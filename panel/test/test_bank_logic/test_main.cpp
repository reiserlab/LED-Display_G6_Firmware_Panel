// Host-side unit tests for the bank-window scan math in src/bank_logic.h
// (LAB-286). Runs on the native platform (no hardware): pixi run test
#include <unity.h>

#include "bank_logic.h"

using namespace Bank;

void setUp() {}
void tearDown() {}

// --- bank masks --------------------------------------------------------------

void test_masks_partition_the_20_columns() {
  uint32_t all = 0;
  for (int k = 0; k < kNumBanks; k++) {
    TEST_ASSERT_EQUAL_UINT32(0, all & kBankMask[k]);  // disjoint
    all |= kBankMask[k];
  }
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFu, all);
}

void test_mask_bank_is_column_mod_4() {
  for (int c = 0; c < 20; c++) {
    for (int k = 0; k < kNumBanks; k++) {
      bool in = (kBankMask[k] >> c) & 1u;
      TEST_ASSERT_EQUAL(c % 4 == k, in);
    }
  }
}

// --- window table limits -----------------------------------------------------

void test_option_b_defaults_are_valid() {
  TEST_ASSERT_EQUAL_UINT32(44900, window_total_ns(kDefaultWindowNs));
  TEST_ASSERT_TRUE(check_windows(kDefaultWindowNs) == WindowCheck::Ok);
}

void test_window_limits() {
  const uint16_t zeros[4] = {0, 0, 0, 0};
  TEST_ASSERT_TRUE(check_windows(zeros) == WindowCheck::Ok);
  const uint16_t one_full_row[4] = {45000, 0, 0, 0};
  TEST_ASSERT_TRUE(check_windows(one_full_row) == WindowCheck::Ok);
  const uint16_t too_long[4] = {45001, 0, 0, 0};
  TEST_ASSERT_TRUE(check_windows(too_long) == WindowCheck::WindowTooLong);
  const uint16_t at_total[4] = {12000, 12000, 12000, 12000};
  TEST_ASSERT_TRUE(check_windows(at_total) == WindowCheck::Ok);
  const uint16_t over_total[4] = {12000, 12000, 12000, 12001};
  TEST_ASSERT_TRUE(check_windows(over_total) == WindowCheck::TotalTooLong);
}

// --- cycle math --------------------------------------------------------------

void test_window_cycles_at_150_mhz() {
  TEST_ASSERT_EQUAL_UINT32(630, window_cycles(4200, 150, 1.0f));
  TEST_ASSERT_EQUAL_UINT32(825, window_cycles(5500, 150, 1.0f));
  TEST_ASSERT_EQUAL_UINT32(1905, window_cycles(12700, 150, 1.0f));
  TEST_ASSERT_EQUAL_UINT32(3375, window_cycles(22500, 150, 1.0f));
  TEST_ASSERT_EQUAL_UINT32(0, window_cycles(0, 150, 1.0f));
}

void test_window_cycles_scale_with_base() {
  // Triggered: 1.0 µs base / 3.0 µs table base.
  TEST_ASSERT_EQUAL_UINT32(210, window_cycles(4200, 150, 1.0f / 3.0f));
  TEST_ASSERT_EQUAL_UINT32(1125, window_cycles(22500, 150, 1.0f / 3.0f));
}

void test_gray2_plane_is_window_times_duty() {
  TEST_ASSERT_EQUAL_UINT32(630, plane_on_cycles(630, 15, 15, 255));
  TEST_ASSERT_EQUAL_UINT32(316, plane_on_cycles(630, 15, 15, 128));  // 316.2
  TEST_ASSERT_EQUAL_UINT32(0, plane_on_cycles(630, 15, 15, 0));
}

void test_gray16_planes_split_the_window() {
  // Violet at duty 255: LSB = 630 / 15 = 42 cycles (280 ns).
  TEST_ASSERT_EQUAL_UINT32(42, plane_on_cycles(630, 1, 15, 255));
  TEST_ASSERT_EQUAL_UINT32(84, plane_on_cycles(630, 2, 15, 255));
  TEST_ASSERT_EQUAL_UINT32(168, plane_on_cycles(630, 4, 15, 255));
  TEST_ASSERT_EQUAL_UINT32(336, plane_on_cycles(630, 8, 15, 255));
  // Full intensity sums to the window within the per-plane rounding.
  for (int duty = 1; duty <= 255; duty++) {
    const uint32_t weights[4] = {1, 2, 4, 8};
    uint32_t sum = 0;
    for (uint32_t w : weights) sum += plane_on_cycles(3375, w, 15, (uint8_t)duty);
    double want = 3375.0 * duty / 255.0;
    TEST_ASSERT_TRUE(sum >= want - 2.0 && sum <= want + 2.0);
  }
}

void test_pio_delay_floor() {
  TEST_ASSERT_EQUAL_UINT32(37, pio_delay(42));
  TEST_ASSERT_EQUAL_UINT32(0, pio_delay(5));
  TEST_ASSERT_EQUAL_UINT32(0, pio_delay(1));   // floor: the plane still lights 5 cycles
  TEST_ASSERT_EQUAL_UINT32(0, pio_delay(0));
}

void test_plane_order_is_bank_major() {
  TEST_ASSERT_EQUAL(0, plane_index(0, 0, 1));
  TEST_ASSERT_EQUAL(3, plane_index(3, 0, 1));
  TEST_ASSERT_EQUAL(4, plane_index(1, 0, 4));
  TEST_ASSERT_EQUAL(15, plane_index(3, 3, 4));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_masks_partition_the_20_columns);
  RUN_TEST(test_mask_bank_is_column_mod_4);
  RUN_TEST(test_option_b_defaults_are_valid);
  RUN_TEST(test_window_limits);
  RUN_TEST(test_window_cycles_at_150_mhz);
  RUN_TEST(test_window_cycles_scale_with_base);
  RUN_TEST(test_gray2_plane_is_window_times_duty);
  RUN_TEST(test_gray16_planes_split_the_window);
  RUN_TEST(test_pio_delay_floor);
  RUN_TEST(test_plane_order_is_bank_major);
  return UNITY_END();
}
