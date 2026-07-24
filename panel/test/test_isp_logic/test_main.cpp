// Host-side unit tests for the pure ISP logic in src/isp_logic.h.
// Runs on the native platform (no hardware): pixi run test
#include <string.h>

#include <unity.h>

#include "isp_logic.h"

using namespace Isp;

void setUp() {}
void tearDown() {}

// --- progress_cols -----------------------------------------------------------

void test_progress_empty_is_zero() {
  TEST_ASSERT_EQUAL_UINT8(0, progress_cols(0));
}

void test_progress_monotonic_below_nominal() {
  uint8_t prev = 0;
  for (uint32_t pages = 0; pages <= kNominalPages; ++pages) {
    uint8_t cols = progress_cols(pages);
    TEST_ASSERT_TRUE(cols >= prev);
    prev = cols;
  }
}

void test_progress_full_at_nominal() {
  TEST_ASSERT_EQUAL_UINT8(kUploadCols, progress_cols(kNominalPages));
}

void test_progress_real_images_never_wrap() {
  // Real images are ~96-140 KB (~380..550 pages); the upload segment must
  // fill near-linearly and never fall back during a normal flash.
  uint8_t prev = 0;
  for (uint32_t pages = 0; pages <= 550; ++pages) {
    uint8_t cols = progress_cols(pages);
    TEST_ASSERT_TRUE(cols >= prev);
    prev = cols;
  }
  TEST_ASSERT_EQUAL_UINT8(15, progress_cols(550));  // 140.8 KB: near segment end
}

void test_progress_never_exceeds_upload_segment() {
  for (uint32_t pages = 0; pages <= kMaxPages; ++pages) {
    TEST_ASSERT_TRUE(progress_cols(pages) <= kUploadCols);
  }
}

void test_progress_wraps_past_nominal() {
  // Oversize images fall back to indeterminate wrap-and-refill WITHIN the
  // upload segment (612 pages -> filled 17 -> wraps to 1).
  TEST_ASSERT_EQUAL_UINT8(kUploadCols, progress_cols(605));  // still in segment
  TEST_ASSERT_EQUAL_UINT8(1, progress_cols(612));
  TEST_ASSERT_EQUAL_UINT8(0, progress_cols(2 * kNominalPages));
}

// --- commit_progress_cols ----------------------------------------------------

void test_segment_layout() {
  TEST_ASSERT_TRUE(0 < kUploadCols);
  TEST_ASSERT_TRUE(kUploadCols < kVerifyEntryCols);
  TEST_ASSERT_TRUE(kVerifyEntryCols <= PANEL_SIZE);
}

void test_commit_progress_endpoints() {
  TEST_ASSERT_EQUAL_UINT8(kVerifyEntryCols, commit_progress_cols(0, 130048));
  TEST_ASSERT_EQUAL_UINT8(PANEL_SIZE, commit_progress_cols(130048, 130048));
}

void test_commit_progress_monotonic_chunks() {
  // Sweep in the real 4096-byte staging chunks over totals that do not divide
  // evenly by the 3-column commit segment: nondecreasing, stays in
  // [kVerifyEntryCols, PANEL_SIZE], hits PANEL_SIZE exactly at written==total.
  const uint32_t totals[] = {130048, 100000, 98304};
  for (uint32_t total : totals) {
    uint8_t prev = kVerifyEntryCols;
    for (uint32_t written = 0; written < total;) {
      uint32_t chunk = (total - written < 4096) ? (total - written) : 4096;
      written += chunk;
      uint8_t cols = commit_progress_cols(written, total);
      TEST_ASSERT_TRUE(cols >= prev);
      TEST_ASSERT_TRUE(cols >= kVerifyEntryCols);
      TEST_ASSERT_TRUE(cols <= PANEL_SIZE);
      if (written == total) TEST_ASSERT_EQUAL_UINT8(PANEL_SIZE, cols);
      if (written < total) TEST_ASSERT_TRUE(cols < PANEL_SIZE);
      prev = cols;
    }
  }
}

void test_commit_progress_degenerate() {
  TEST_ASSERT_EQUAL_UINT8(PANEL_SIZE, commit_progress_cols(0, 0));
  TEST_ASSERT_EQUAL_UINT8(PANEL_SIZE, commit_progress_cols(5000, 4096));  // clamps
  // Largest legal image: no uint32 overflow in the segment math.
  TEST_ASSERT_EQUAL_UINT8(PANEL_SIZE - 1, commit_progress_cols(kStageMax - 1, kStageMax));
}

// --- write_page_in_bounds ----------------------------------------------------

void test_write_page_bounds() {
  TEST_ASSERT_TRUE(write_page_in_bounds(0));
  TEST_ASSERT_TRUE(write_page_in_bounds(kMaxPages - 1));
  TEST_ASSERT_FALSE(write_page_in_bounds(kMaxPages));
}

void test_write_page_rejects_wrapping_index() {
  // 0xFFFFFF is the largest 24-bit index the wire format can carry;
  // 0xFFFFFF * 256 wraps uint32, so the old offset-based check passed it.
  TEST_ASSERT_FALSE(write_page_in_bounds(0xFFFFFFu));
  // Smallest index whose byte offset wraps uint32.
  TEST_ASSERT_FALSE(write_page_in_bounds(0x1000000u));
}

// --- retires_boot_indicator --------------------------------------------------

void test_retire_on_display_commands() {
  const uint8_t content[] = {
      CMD_ID_DISPLAY_GRAY_2,      CMD_ID_DISPLAY_GRAY_2_PERSIST,
      CMD_ID_DISPLAY_GRAY_2_TRIGGERED, CMD_ID_DISPLAY_GRAY_2_GATED,
      CMD_ID_DISPLAY_GRAY_16,     CMD_ID_DISPLAY_GRAY_16_PERSIST,
      CMD_ID_DISPLAY_GRAY_16_TRIGGERED, CMD_ID_DISPLAY_GRAY_16_GATED,
      CMD_ID_DISPLAY_PSRAM,       CMD_ID_DISPLAY_PSRAM_PERSIST,
      CMD_ID_DISPLAY_PSRAM_TRIGGERED, CMD_ID_DISPLAY_PSRAM_GATED,
      CMD_ID_DISPLAY_PSRAM_DUTY,  CMD_ID_DISPLAY_PSRAM_DUTY_PERSIST,
      CMD_ID_DISPLAY_PSRAM_DUTY_TRIGGERED, CMD_ID_DISPLAY_PSRAM_DUTY_GATED,
  };
  for (uint8_t cmd : content) {
    TEST_ASSERT_TRUE_MESSAGE(retires_boot_indicator(cmd),
                             "display command must retire the indicator");
  }
}

void test_no_retire_on_non_content_commands() {
  TEST_ASSERT_FALSE(retires_boot_indicator(CMD_ID_COMMS_CHECK));
  TEST_ASSERT_FALSE(retires_boot_indicator(CMD_ID_ERROR_DISPLAY));
  const uint8_t isp[] = {
      CMD_ID_ISP_ENTER,  CMD_ID_ISP_WRITE_PAGE, CMD_ID_ISP_VERIFY_STAGED,
      CMD_ID_ISP_COMMIT, CMD_ID_ISP_VERIFY_CRC, CMD_ID_ISP_EXIT_REBOOT,
  };
  for (uint8_t cmd : isp) {
    TEST_ASSERT_FALSE_MESSAGE(retires_boot_indicator(cmd),
                              "ISP opcode must not retire the indicator");
  }
}

// --- glyph geometry ----------------------------------------------------------

void assert_glyph_well_formed(const char *const rows[PANEL_SIZE]) {
  for (int r = 0; r < PANEL_SIZE; ++r) {
    TEST_ASSERT_NOT_NULL(rows[r]);
    TEST_ASSERT_EQUAL_size_t(PANEL_SIZE, strlen(rows[r]));
    for (int c = 0; c < PANEL_SIZE; ++c) {
      TEST_ASSERT_TRUE(rows[r][c] == '.' || rows[r][c] == '#');
    }
  }
}

int count_lit(const char *const rows[PANEL_SIZE]) {
  int lit = 0;
  for (int r = 0; r < PANEL_SIZE; ++r)
    for (int c = 0; c < PANEL_SIZE; ++c)
      if (rows[r][c] == '#') ++lit;
  return lit;
}

void assert_glyph_left_right_symmetric(const char *const rows[PANEL_SIZE]) {
  for (int r = 0; r < PANEL_SIZE; ++r)
    for (int c = 0; c < PANEL_SIZE; ++c)
      TEST_ASSERT_EQUAL_CHAR(rows[r][c], rows[r][PANEL_SIZE - 1 - c]);
}

void test_smiley_well_formed() {
  assert_glyph_well_formed(kSmiley);
  assert_glyph_left_right_symmetric(kSmiley);
  TEST_ASSERT_TRUE(count_lit(kSmiley) > 0);
}

void test_sad_smiley_well_formed() {
  assert_glyph_well_formed(kSadSmiley);
  assert_glyph_left_right_symmetric(kSadSmiley);
  TEST_ASSERT_TRUE(count_lit(kSadSmiley) > 0);
}

void test_faces_are_distinct() {
  bool differ = false;
  for (int r = 0; r < PANEL_SIZE && !differ; ++r)
    differ = strcmp(kSmiley[r], kSadSmiley[r]) != 0;
  TEST_ASSERT_TRUE_MESSAGE(differ, "success and failure faces must differ");
}

void test_bar_rows_inside_panel() {
  TEST_ASSERT_TRUE(0 <= kBarRowFirst);
  TEST_ASSERT_TRUE(kBarRowFirst <= kBarRowLast);
  TEST_ASSERT_TRUE(kBarRowLast < PANEL_SIZE);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_progress_empty_is_zero);
  RUN_TEST(test_progress_monotonic_below_nominal);
  RUN_TEST(test_progress_full_at_nominal);
  RUN_TEST(test_progress_real_images_never_wrap);
  RUN_TEST(test_progress_never_exceeds_upload_segment);
  RUN_TEST(test_progress_wraps_past_nominal);
  RUN_TEST(test_segment_layout);
  RUN_TEST(test_commit_progress_endpoints);
  RUN_TEST(test_commit_progress_monotonic_chunks);
  RUN_TEST(test_commit_progress_degenerate);
  RUN_TEST(test_write_page_bounds);
  RUN_TEST(test_write_page_rejects_wrapping_index);
  RUN_TEST(test_retire_on_display_commands);
  RUN_TEST(test_no_retire_on_non_content_commands);
  RUN_TEST(test_smiley_well_formed);
  RUN_TEST(test_sad_smiley_well_formed);
  RUN_TEST(test_faces_are_distinct);
  RUN_TEST(test_bar_rows_inside_panel);
  return UNITY_END();
}
