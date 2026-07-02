#include "isp.h"

#include <Arduino.h>
#include <string.h>
#include <hardware/watchdog.h>
#include <hardware/regs/addressmap.h>  // XIP_BASE
#include <pico/time.h>
#include <LittleFS.h>
#include <PicoOTA.h>

#include "protocol.h"
#include "panel_spi_custom.h"
#include "pattern.h"
#include "pico/util/queue.h"

// Display queue (core 0 -> core 1), owned by main.cpp. The ISP receiver pushes
// Persistent progress/indicator Patterns into it exactly like Messenger does;
// Display::update()'s drain-to-latest keeps it shallow.
extern queue_t display_queue;

namespace {

// --- Constants (MUST match the controller's IspController) ------------------
const char        kSentinel[]  = "G6PANELISPENTER";   // 15 chars + NUL = 16 bytes
constexpr uint8_t kUnlock[4]   = {'I', 'S', 'P', '!'};
constexpr uint32_t kStageMax   = 2u * 1024u * 1024u;  // ≤ RP2354 2 MiB app flash
constexpr uint16_t kPageBytes  = 256;                 // flash program granule (WRITE_PAGE)
constexpr uint32_t kSectorBytes = 4096;               // flash erase granule (reported in ENTER reply)
const char kImageFile[] = "/firmware.bin";            // LittleFS staging file for the OTA stub
const char kFlashedFlag[] = "/just_flashed";          // marker: OTA staged, boot indicator due

// --- Visual programming indicator (LAB-44) ----------------------------------
// While pages stream in, the panel shows a progress bar across the central 10
// rows (rows 5..14), filling left to right. The panel does not know the image
// total up front (only the controller does), so the bar is scaled to a nominal
// image size; a larger image simply wraps and refills — indeterminate fallback
// for free. Real images (~96-140 KB -> ~380..540 pages) fill near-linearly.
constexpr uint32_t kNominalPages = 512;  // ~128 KiB nominal image
constexpr int      kBarRowFirst  = 5;
constexpr int      kBarRowLast   = 14;
constexpr uint8_t  kIndicatorDuty = 128;

uint8_t bar_cols_shown_ = 0xFF;   // last pushed fill (0xFF = none this session)

// 20x20 smiley, row 0 = top (same Pattern orientation the predef glyphs use).
// Shown by boot_indicator_check() on the first boot after an ISP flash.
const char *kSmiley[PANEL_SIZE] = {
    "....................",
    "....................",
    "....................",
    "....................",
    ".....##......##.....",
    ".....##......##.....",
    "....................",
    "....................",
    "....................",
    "....................",
    "...#............#...",
    "....#..........#....",
    ".....#........#.....",
    "......########......",
    "....................",
    "....................",
    "....................",
    "....................",
    "....................",
    "....................",
};

// Push a Persistent Gray_2 progress frame with `cols` columns lit (0..20).
void push_progress(uint8_t cols) {
  Pattern pat;
  pat.set_gray_level(GrayLevel::Gray_2);
  pat.set_duty_cycle(kIndicatorDuty);
  pat.set_mode(DisplayMode::Persistent);
  pat.matrix().setZero();
  for (int r = kBarRowFirst; r <= kBarRowLast; ++r)
    for (int c = 0; c < cols && c < PANEL_SIZE; ++c)
      pat.matrix()(r, c) = 1;
  queue_try_add(&display_queue, &pat);  // drop-on-full is fine (drain-to-latest)
}

// update_progress() lives below the session-state block (uses image_len_).

// --- ISP session state ------------------------------------------------------
uint8_t  *stage_       = nullptr;   // PSRAM staging buffer (pmalloc, lazy)
uint32_t  nonce_       = 0;
bool      armed_       = false;     // ISP_ENTER accepted
uint32_t  image_len_   = 0;

// --- Reply state ------------------------------------------------------------
uint8_t  resp_buf_[20];
uint8_t  resp_len_     = 0;
bool     resp_pending_ = false;
bool     reboot_due_   = false;     // reboot after the COMMIT receipt clocks out
bool     fs_ready_     = false;     // LittleFS mounted

// Recompute bar fill from bytes staged so far; push only on a column change.
void update_progress() {
  uint32_t pages  = image_len_ / kPageBytes;
  uint32_t filled = pages * PANEL_SIZE / kNominalPages;
  uint8_t  shown  = (filled <= PANEL_SIZE) ? (uint8_t)filled
                                           : (uint8_t)(filled % PANEL_SIZE);
  if (shown != bar_cols_shown_) {
    bar_cols_shown_ = shown;
    push_progress(shown);
  }
}

// CRC-8/AUTOSAR (poly 0x2F, init 0xFF, xorout 0xFF) — matches G6::crc8_autosar.
uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0xFF;
  for (size_t i = 0; i < n; ++i) {
    c ^= d[i];
    for (int b = 0; b < 8; ++b) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x2F) : (uint8_t)(c << 1);
  }
  return c ^ 0xFF;
}

// CRC-32/ISO-HDLC (zlib), 4-bit table — matches G6::crc32_update on the controller.
uint32_t crc32_update(uint32_t c, const uint8_t *d, size_t n) {
  static const uint32_t T[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u,
    0x4db26158u, 0x5005713cu, 0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu};
  for (size_t i = 0; i < n; ++i) {
    c = T[(c ^ d[i]) & 0xf] ^ (c >> 4);
    c = T[(c ^ (d[i] >> 4)) & 0xf] ^ (c >> 4);
  }
  return c;
}

uint32_t crc32(const uint8_t *d, size_t n) {
  return crc32_update(0xFFFFFFFFu, d, n) ^ 0xFFFFFFFFu;
}

uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint32_t rd24(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

// Build reply [status][payload...][crc8] into resp_buf_ and mark it pending.
void arm(uint8_t status, const uint8_t *payload, uint8_t plen) {
  resp_buf_[0] = status;
  if (plen) memcpy(resp_buf_ + 1, payload, plen);
  resp_buf_[1 + plen] = crc8(resp_buf_, 1 + plen);
  resp_len_ = (uint8_t)(2 + plen);
  resp_pending_ = true;
}

// Mount LittleFS (format on first use). LittleFS flash ops are multicore-safe via
// the core's idleOtherCore() (a RAM-resident doorbell IRQ parks core 1).
bool ensure_fs() {
  if (fs_ready_) return true;
  if (LittleFS.begin()) { fs_ready_ = true; return true; }
  Serial.println("[isp] LittleFS unformatted — formatting (one-time)");
  if (LittleFS.format() && LittleFS.begin()) { fs_ready_ = true; return true; }
  Serial.println("[isp] LittleFS mount FAILED");
  return false;
}

// Commit via the core's built-in OTA path instead of reprogramming the running
// image at offset 0 (do_commit, removed — it bricked because rebooting into a
// self-rewritten image FROM the running app fails; the scratch probe proved the
// flash WRITE itself is byte-correct). Here we dump the verified PSRAM image into
// a LittleFS file and register an OTA command; on the next boot the core's OTA
// stub (lib/<chip>/ota.o, linked at flash offset 0) copies it into the app region
// from a minimal early-boot state, verifies, and boots. otacommand.bin persists
// until the copy succeeds, so a mid-copy power loss self-heals on the next boot.
bool stage_image_to_ota() {
  if (!ensure_fs()) return false;
  File f = LittleFS.open(kImageFile, "w");
  if (!f) { Serial.println("[isp] open firmware.bin failed"); return false; }
  // PSRAM -> SRAM -> file in bursts (per-byte PSRAM reads are slow).
  static uint8_t buf[4096];
  for (uint32_t off = 0; off < image_len_;) {
    uint32_t chunk = (image_len_ - off < sizeof(buf)) ? (image_len_ - off) : (uint32_t)sizeof(buf);
    memcpy(buf, stage_ + off, chunk);
    if (f.write(buf, chunk) != chunk) { f.close(); Serial.println("[isp] fs write short"); return false; }
    off += chunk;
  }
  f.close();
  // Register the copy: firmware.bin (whole file) -> XIP_BASE (app region).
  picoOTA.begin();
  if (!picoOTA.addFile(kImageFile, 0, XIP_BASE, image_len_)) { Serial.println("[isp] addFile failed"); return false; }
  if (!picoOTA.commit()) { Serial.println("[isp] otacommand commit failed"); return false; }
  return true;
}

}  // namespace

namespace Isp {

void init() {
  // Reserve the staging buffer now so ISP_ENTER is a fast handshake (no pmalloc
  // in the critical phase-A → phase-B window). PSRAM is 8 MiB; 2 MiB reserved is
  // fine (v2 PSRAM patterns are not implemented in this build).
  if (!stage_) stage_ = (uint8_t *)pmalloc(kStageMax);
  // NB: do NOT mount/format LittleFS here. init() runs inside messenger.initialize()
  // during setup() — before loop() and before both cores are servicing their loops —
  // and LittleFS flash writes (format) use idleOtherCore(), which deadlocks/hangs
  // that early. ensure_fs() is called lazily at ISP_COMMIT (loop context) instead.
}

bool response_pending() { return resp_pending_; }

void service_pending() {
  panel_spi_drive_response(resp_buf_, resp_len_);
  resp_pending_ = false;
  if (reboot_due_) {
    reboot_due_ = false;
    // Image is staged in LittleFS + otacommand.bin; reboot so the core's OTA stub
    // (flash offset 0) copies it into the app region at clean early-boot.
    watchdog_reboot(0, 0, 10);
    while (true) { tight_loop_contents(); }
  }
}

void handle(Message &msg) {
  const uint8_t *pl = msg.data_ptr() + HEADER_SIZE;  // payload (length already gated)
  const uint8_t cmd = msg.command_byte();

  switch (cmd) {
    case CMD_ID_ISP_ENTER: {
      if (memcmp(pl, kSentinel, 16) != 0 || memcmp(pl + 16, kUnlock, 4) != 0) {
        arm(1, nullptr, 0);  // bad sentinel/token
        return;
      }
      if (!stage_) stage_ = (uint8_t *)pmalloc(kStageMax);
      if (!stage_) { arm(2, nullptr, 0); return; }  // out of PSRAM
      nonce_ = time_us_32() ^ 0xA5A5A5A5u;          // session nonce (no RNG dep)
      armed_ = true;
      image_len_ = 0;
      bar_cols_shown_ = 0xFF;   // new session: first WRITE_PAGE pushes a fresh bar
                                // (no push here — the ENTER reply window is tight)
      uint8_t p[17];
      memcpy(p, &nonce_, 4);
      uint32_t fs = kStageMax;  memcpy(p + 4, &fs, 4);
      uint16_t ps = kPageBytes; memcpy(p + 8, &ps, 2);
      uint16_t ss = (uint16_t)kSectorBytes; memcpy(p + 10, &ss, 2);
      uint32_t app_crc = 0; memcpy(p + 12, &app_crc, 4);  // TODO(bench): running-app CRC
      p[16] = 0;  // bootrom_version placeholder
      arm(0, p, 17);
      return;
    }

    case CMD_ID_ISP_WRITE_PAGE: {
      if (!armed_) { arm(1, nullptr, 0); return; }
      uint32_t idx  = rd24(pl);
      uint32_t n    = rd32(pl + 3);
      if (n != nonce_) { arm(2, nullptr, 0); return; }
      const uint8_t *data = pl + 7;
      uint32_t pcrc = rd32(pl + 7 + kPageBytes);
      if (crc32(data, kPageBytes) != pcrc) { arm(3, nullptr, 0); return; }
      uint32_t off = idx * kPageBytes;
      if (off + kPageBytes > kStageMax) { arm(4, nullptr, 0); return; }
      memcpy(stage_ + off, data, kPageBytes);
      if (off + kPageBytes > image_len_) image_len_ = off + kPageBytes;
      arm(0, nullptr, 0);
      update_progress();   // throttled: pushes only when the bar gains a column
      return;
    }

    case CMD_ID_ISP_VERIFY_STAGED: {
      if (!armed_) { arm(1, nullptr, 0); return; }
      uint32_t n   = rd32(pl);
      uint32_t len = rd24(pl + 4);
      uint32_t exp = rd32(pl + 7);
      if (n != nonce_) { arm(2, nullptr, 0); return; }
      if (len == 0 || len > kStageMax) { arm(4, nullptr, 0); return; }
      image_len_ = len;
      // Byte-by-byte PSRAM read (memcpy bursts mis-read this PSRAM at speed).
      uint32_t got = crc32(stage_, len);
      uint8_t p[4]; memcpy(p, &got, 4);
      arm(got == exp ? 0 : 5, p, 4);  // 5 = staged CRC mismatch
      return;
    }

    case CMD_ID_ISP_COMMIT: {
      if (!armed_) { arm(1, nullptr, 0); return; }
      uint32_t n   = rd32(pl);
      uint32_t len = rd24(pl + 4);
      if (n != nonce_) { arm(2, nullptr, 0); return; }
      if (len == 0 || len > kStageMax) { arm(4, nullptr, 0); return; }
      image_len_ = len;
      push_progress(PANEL_SIZE);     // full bar while OTA staging writes flash
      // Stage the verified image into LittleFS + an OTA command, then reboot so the
      // core's OTA stub flashes it at clean early-boot (see stage_image_to_ota).
      bool ok = stage_image_to_ota();
      if (ok) {
        // Marker for the freshly-flashed image: its first boot shows the smiley
        // boot indicator (boot_indicator_check); cleared again on the first
        // host display command (notify_host_command).
        File f = LittleFS.open(kFlashedFlag, "w");
        if (f) { f.write((uint8_t)'1'); f.close(); }
      }
      arm(ok ? 0 : 8, nullptr, 0);   // status 8 = OTA staging failed
      if (ok) reboot_due_ = true;    // reboot after the receipt clocks out
      return;
    }

    case CMD_ID_ISP_VERIFY_CRC: {
      uint32_t start = rd24(pl);
      uint32_t len   = rd24(pl + 3);
      uint32_t n     = rd32(pl + 6);
      uint32_t exp   = rd32(pl + 10);
      if (n != nonce_) { arm(2, nullptr, 0); return; }
      const uint8_t *flash = (const uint8_t *)(XIP_BASE + start);  // programmed flash
      uint32_t got = crc32(flash, len);
      uint8_t p[4]; memcpy(p, &got, 4);
      arm(got == exp ? 0 : 5, p, 4);  // 5 = flash CRC mismatch
      return;
    }

    case CMD_ID_ISP_EXIT_REBOOT: {
      uint32_t n = rd32(pl);
      if (n != nonce_) return;          // ignore a mismatched-nonce reboot request
      watchdog_reboot(0, 0, 10);        // reboot into the (new) application
      while (true) { tight_loop_contents(); }
    }

    default:
      return;
  }
}

void boot_indicator_check() {
  // Called once from the production loop() on core 0, after both cores are in
  // steady state — never from setup(): LittleFS flash writes/format use
  // idleOtherCore(), which hangs before core 1 services its loop.
  //
  // Mount WITHOUT auto-format: on a panel that has never been ISP-flashed the
  // FS region is unformatted and must stay untouched here (formatting is
  // ensure_fs()'s job, lazily, in ISP context).
  LittleFSConfig cfg(false);   // autoFormat = false
  LittleFS.setConfig(cfg);
  if (!LittleFS.begin()) return;
  fs_ready_ = true;
  if (!LittleFS.exists(kFlashedFlag)) return;

  // First boot after an ISP flash: show the smiley (Persistent) until the
  // first host display command replaces it. The flag is retired on that first
  // command (notify_host_command), so the indicator also survives power
  // cycles until the panel is actually used.
  Pattern pat;
  pat.set_gray_level(GrayLevel::Gray_2);
  pat.set_duty_cycle(kIndicatorDuty);
  pat.set_mode(DisplayMode::Persistent);
  pat.matrix().setZero();
  for (int r = 0; r < PANEL_SIZE; ++r)
    for (int c = 0; c < PANEL_SIZE; ++c)
      if (kSmiley[r][c] == '#') pat.matrix()(r, c) = 1;
  queue_try_add(&display_queue, &pat);
}

void notify_host_command() {
  // First valid host display command: retire the boot-indicator flag so the
  // smiley doesn't reappear on the next power cycle. Loop context on core 0
  // with core 1 live — safe for a LittleFS metadata write.
  static bool done = false;
  if (done) return;
  done = true;
  if (fs_ready_ && LittleFS.exists(kFlashedFlag)) LittleFS.remove(kFlashedFlag);
}

}  // namespace Isp
