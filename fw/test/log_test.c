/**
 * PET Clone - Open hardware implementation of the Commodore PET
 * by Daniel Lehenbauer and contributors.
 *
 * https://github.com/DLehenbauer/commodore-pet-clone
 *
 * To the extent possible under law, I, Daniel Lehenbauer, have waived all
 * copyright and related or neighboring rights to this project. This work is
 * published from the United States.
 *
 * @copyright CC0 http://creativecommons.org/publicdomain/zero/1.0/
 * @author Daniel Lehenbauer <DLehenbauer@users.noreply.github.com> and contributors
 */

#include "pch.h"
#include "log_test.h"
#include "../src/diag/log/log.h"

START_TEST(test_log_init) {
    log_init();
    
    // After init, there should be no entries
    ck_assert_uint_eq(log_entry_count(LOG_LEVEL_DEBUG), 0);
    ck_assert_uint_eq(log_entry_count(LOG_LEVEL_INFO), 0);
    ck_assert_uint_eq(log_entry_count(LOG_LEVEL_WARN), 0);
} END_TEST

START_TEST(test_log_single_entry) {
    log_init();
    
    log_info("Test message");
    
    // log_entry_count returns count of entries at or above the given level
    ck_assert_uint_eq(log_entry_count(LOG_LEVEL_DEBUG), 1); // INFO is above DEBUG
    ck_assert_uint_eq(log_entry_count(LOG_LEVEL_INFO), 1);  // INFO level
    ck_assert_uint_eq(log_entry_count(LOG_LEVEL_WARN), 0);  // No WARN entries
} END_TEST

START_TEST(test_log_multiple_levels) {
    log_init();
    
    log_debug("Debug message");
    log_info("Info message");
    log_warn("Warn message");
    
    // Each level should have 1 entry
    ck_assert_uint_eq(log_entry_count(LOG_LEVEL_DEBUG), 3); // DEBUG includes all levels
    ck_assert_uint_eq(log_entry_count(LOG_LEVEL_INFO), 2);  // INFO includes INFO + WARN
    ck_assert_uint_eq(log_entry_count(LOG_LEVEL_WARN), 1);  // WARN only
} END_TEST

START_TEST(test_log_wraparound) {
    log_init();
    
    // Fill the WARN buffer and add 4 more to verify wrap-around
    for (int i = 0; i < LOG_WARN_ENTRIES + 4; i++) {
        log_warn("Message %d", i);
    }
    
    // Count should be capped at capacity
    ck_assert_uint_eq(log_entry_count(LOG_LEVEL_WARN), LOG_WARN_ENTRIES);
    
    // Iterate and verify we get the most recent entries (the oldest 4 were overwritten)
    log_iterator_t iter;
    log_iter_init(&iter, LOG_LEVEL_WARN);
    
    const log_entry_t* entry;
    int expected_msg_num = 4; // First 4 (0-3) were overwritten
    
    while ((entry = log_iter_next(&iter, NULL)) != NULL) {
        char expected_msg[LOG_MESSAGE_LENGTH];
        snprintf(expected_msg, sizeof(expected_msg), "Message %d", expected_msg_num);
        ck_assert_str_eq(entry->message, expected_msg);
        expected_msg_num++;
    }
    
    // We should have read exactly LOG_WARN_ENTRIES messages
    ck_assert_int_eq(expected_msg_num, LOG_WARN_ENTRIES + 4);
} END_TEST

START_TEST(test_log_iterator_chronological) {
    log_init();
    
    // Add entries across different levels
    log_debug("Debug 1");
    log_info("Info 1");
    log_warn("Warn 1");
    log_debug("Debug 2");
    log_info("Info 2");
    
    // Iterate all entries - they should come out in chronological order
    log_iterator_t iter;
    log_iter_init(&iter, LOG_LEVEL_DEBUG);
    
    const log_entry_t* entry;
    log_level_t level;
    
    entry = log_iter_next(&iter, &level);
    ck_assert_ptr_nonnull(entry);
    ck_assert_str_eq(entry->message, "Debug 1");
    ck_assert_int_eq(level, LOG_LEVEL_DEBUG);
    
    entry = log_iter_next(&iter, &level);
    ck_assert_ptr_nonnull(entry);
    ck_assert_str_eq(entry->message, "Info 1");
    ck_assert_int_eq(level, LOG_LEVEL_INFO);
    
    entry = log_iter_next(&iter, &level);
    ck_assert_ptr_nonnull(entry);
    ck_assert_str_eq(entry->message, "Warn 1");
    ck_assert_int_eq(level, LOG_LEVEL_WARN);
    
    entry = log_iter_next(&iter, &level);
    ck_assert_ptr_nonnull(entry);
    ck_assert_str_eq(entry->message, "Debug 2");
    ck_assert_int_eq(level, LOG_LEVEL_DEBUG);
    
    entry = log_iter_next(&iter, &level);
    ck_assert_ptr_nonnull(entry);
    ck_assert_str_eq(entry->message, "Info 2");
    ck_assert_int_eq(level, LOG_LEVEL_INFO);
    
    entry = log_iter_next(&iter, NULL);
    ck_assert_ptr_null(entry);
} END_TEST

START_TEST(test_log_iterator_filtered) {
    log_init();
    
    log_debug("Debug 1");
    log_info("Info 1");
    log_warn("Warn 1");
    
    // Iterate only WARN entries
    log_iterator_t iter;
    log_iter_init(&iter, LOG_LEVEL_WARN);
    
    const log_entry_t* entry;
    
    entry = log_iter_next(&iter, NULL);
    ck_assert_ptr_nonnull(entry);
    ck_assert_str_eq(entry->message, "Warn 1");
    
    entry = log_iter_next(&iter, NULL);
    ck_assert_ptr_null(entry);
} END_TEST

START_TEST(test_log_message_formatting) {
    log_init();
    
    log_info("Value: %d, String: %s", 42, "hello");
    
    log_iterator_t iter;
    log_iter_init(&iter, LOG_LEVEL_INFO);
    
    const log_entry_t* entry = log_iter_next(&iter, NULL);
    ck_assert_ptr_nonnull(entry);
    ck_assert_str_eq(entry->message, "Value: 42, String: hello");
} END_TEST

START_TEST(test_log_message_truncation) {
    log_init();
    
    // Create a message longer than LOG_MESSAGE_LENGTH
    char long_msg[LOG_MESSAGE_LENGTH + 20];
    memset(long_msg, 'X', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';
    
    log_info("%s", long_msg);
    
    log_iterator_t iter;
    log_iter_init(&iter, LOG_LEVEL_INFO);
    
    const log_entry_t* entry = log_iter_next(&iter, NULL);
    ck_assert_ptr_nonnull(entry);
    
    // Message should be truncated to LOG_MESSAGE_LENGTH - 1 (plus null terminator)
    ck_assert_uint_eq(strlen(entry->message), LOG_MESSAGE_LENGTH - 1);
} END_TEST

Suite *log_suite(void) {
    Suite* s = suite_create("Log");

    TCase* test_cases = tcase_create("log");
    tcase_add_test(test_cases, test_log_init);
    tcase_add_test(test_cases, test_log_single_entry);
    tcase_add_test(test_cases, test_log_multiple_levels);
    tcase_add_test(test_cases, test_log_wraparound);
    tcase_add_test(test_cases, test_log_iterator_chronological);
    tcase_add_test(test_cases, test_log_iterator_filtered);
    tcase_add_test(test_cases, test_log_message_formatting);
    tcase_add_test(test_cases, test_log_message_truncation);

    suite_add_tcase(s, test_cases);

    return s;
}
