// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "fpga_image_test.h"

#include "fpga_image.h"

// CRC-32/ISO-HDLC of "123456789" is 0xcbf43926.
static const uint8_t valid_image[] = {
    'E', 'F', 'P', '1', 1, 0, 0, 0, 9, 0, 0, 0, 0x26, 0x39, 0xf4, 0xcb,
    '1', '2', '3', '4', '5', '6', '7', '8', '9'
};

typedef struct {
    uint8_t data[600];
    size_t length;
    size_t calls;
    size_t fail_on_call;
} writer_t;

// Capture the exact bytes delivered to the FPGA and optionally simulate an SPI failure.
static bool capture_chunk(const uint8_t* data, size_t length, void* context) {
    writer_t* const writer = context;
    writer->calls++;
    if (writer->calls == writer->fail_on_call) return false;
    ck_assert_uint_le(writer->length + length, sizeof(writer->data));
    memcpy(writer->data + writer->length, data, length);
    writer->length += length;
    return true;
}

START_TEST(test_image_validation) {
    uint8_t image[sizeof(valid_image)];
    memcpy(image, valid_image, sizeof(image));
    fpga_image_t payload;
    writer_t writer = {0};
    ck_assert(fpga_image_read_header(image, sizeof(image), &payload));
    ck_assert_ptr_eq(payload.data, image + 16);
    ck_assert_uint_eq(payload.length, 9);
    ck_assert(fpga_image_stream(&payload, capture_chunk, &writer));
    ck_assert_uint_eq(writer.length, 9);
    ck_assert_int_eq(memcmp(writer.data, image + 16, 9), 0);
    ck_assert(!fpga_image_read_header(image, 15, &payload));
    ck_assert(!fpga_image_read_header(image, sizeof(image) - 1, &payload));
    ck_assert_ptr_eq(payload.data, image + 16);
    ck_assert_uint_eq(payload.length, 9);
    image[16] ^= 1;
    writer = (writer_t){0};
    ck_assert(fpga_image_read_header(image, sizeof(image), &payload));
    ck_assert(!fpga_image_stream(&payload, capture_chunk, &writer));
    ck_assert_uint_eq(writer.length, 9);
    ck_assert_int_eq(memcmp(writer.data, image + 16, 9), 0);
    memcpy(image, valid_image, sizeof(image));
    image[0] = 0xff;
    ck_assert(!fpga_image_read_header(image, sizeof(image), &payload));
    memcpy(image, valid_image, sizeof(image));
    image[4] = 2;
    ck_assert(!fpga_image_read_header(image, sizeof(image), &payload));
    memcpy(image, valid_image, sizeof(image));
    memset(image + 8, 0, 4);
    ck_assert(!fpga_image_read_header(image, sizeof(image), &payload));
    memset(image + 8, 0xff, 4);
    ck_assert(!fpga_image_read_header(image, sizeof(image), &payload));
}
END_TEST

// Exercise complete and partial chunks and stop immediately when the writer fails.
START_TEST(test_stream_chunks) {
    uint8_t image[16 + 300] = {
        'E', 'F', 'P', '1', 1, 0, 0, 0, 44, 1, 0, 0, 0, 0, 0, 0
    };
    memset(image + 16, 'a', 300);
    // CRC-32/ISO-HDLC of 300 ASCII 'a' bytes.
    image[12] = 0x09;
    image[13] = 0x19;
    image[14] = 0x97;
    image[15] = 0x89;
    fpga_image_t payload;
    ck_assert(fpga_image_read_header(image, sizeof(image), &payload));

    writer_t writer = {0};
    ck_assert(fpga_image_stream(&payload, capture_chunk, &writer));
    ck_assert_uint_eq(writer.calls, 2);
    ck_assert_uint_eq(writer.length, 300);
    ck_assert_int_eq(memcmp(writer.data, image + 16, 300), 0);

    writer = (writer_t){.fail_on_call = 2};
    ck_assert(!fpga_image_stream(&payload, capture_chunk, &writer));
    ck_assert_uint_eq(writer.calls, 2);
    ck_assert_uint_eq(writer.length, 256);
}
END_TEST

Suite* fpga_image_suite(void) {
    Suite* suite = suite_create("FPGA flash image");
    TCase* tests = tcase_create("validation");
    tcase_add_test(tests, test_image_validation);
    tcase_add_test(tests, test_stream_chunks);
    suite_add_tcase(suite, tests);
    return suite;
}
