// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "tape_dir_test.h"

#include <stdio.h>
#include <string.h>

#include "cbm/filename.h"
#include "cbm/petscii.h"
#include "tape_dir.h"

#define LOAD_ADDR 0x0401

// Disk name passed to the renderer for the header line.
#define DISK_NAME "ECONOPET"

typedef struct {
    uint16_t linenum;
    const uint8_t* text;
    size_t text_len;
} parsed_line_t;

// Walk a rendered directory image, validating link pointers and the $0000
// terminator. Fills 'lines' with each line's number and text span.
static size_t parse_lines(const uint8_t* img, size_t len, uint16_t load_addr,
                          parsed_line_t* lines, size_t max_lines) {
    size_t off = 0;
    size_t n = 0;

    while (off + 2 <= len) {
        uint16_t link = (uint16_t)(img[off] | (img[off + 1] << 8));
        if (link == 0) {
            break;      // terminating $0000 link
        }

        ck_assert(n < max_lines);
        ck_assert(off + 4 <= len);

        uint16_t line_addr = (uint16_t)(load_addr + off);
        uint16_t linenum = (uint16_t)(img[off + 2] | (img[off + 3] << 8));

        size_t t = off + 4;
        while (t < len && img[t] != 0x00) {
            t++;
        }
        ck_assert(t < len);     // text must be null terminated

        lines[n].linenum = linenum;
        lines[n].text = img + off + 4;
        lines[n].text_len = t - (off + 4);

        size_t line_len = (t + 1) - off;    // link + num + text + 00
        ck_assert_uint_eq(link, (uint16_t)(line_addr + line_len));

        n++;
        off = t + 1;
    }

    // The image must end exactly at the terminating $0000 link.
    ck_assert_uint_eq(off + 2, len);
    ck_assert_uint_eq(img[off], 0x00);
    ck_assert_uint_eq(img[off + 1], 0x00);

    return n;
}

static void check_header(const parsed_line_t* line) {
    ck_assert_uint_eq(line->linenum, 0);
    const uint8_t* h = line->text;
    ck_assert_uint_eq(line->text_len, 25);
    ck_assert_uint_eq(h[0], 0x12);          // RVS ON
    ck_assert_uint_eq(h[1], 0x22);          // opening quote
    ck_assert(memcmp(h + 2, "ECONOPET", 8) == 0);
    for (size_t i = 0; i < 8; i++) {
        ck_assert_uint_eq(h[10 + i], ' ');  // pad to 16 chars
    }
    ck_assert_uint_eq(h[18], 0x22);         // closing quote
    ck_assert_uint_eq(h[19], ' ');
    ck_assert(memcmp(h + 20, "SD", 2) == 0); // disk ID
    ck_assert_uint_eq(h[22], ' ');
    ck_assert(memcmp(h + 23, "2A", 2) == 0); // DOS format type
}

// Basic listing: header, two sorted entries, and a footer.
START_TEST(test_render_basic) {
    tape_dir_entry_t entries[] = {
        { "GAME", 5 },
        { "UTIL", 1 },
    };
    uint8_t img[512];
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME, entries, 2, 664 * 254, true);
    ck_assert_uint_gt(len, 0);

    parsed_line_t lines[8];
    size_t n = parse_lines(img, len, LOAD_ADDR, lines, 8);
    ck_assert_uint_eq(n, 4);    // header + 2 files + footer

    check_header(&lines[0]);

    // First file line: "  " padding for block count 5 is 3 leading spaces.
    ck_assert_uint_eq(lines[1].linenum, 5);
    const uint8_t* f = lines[1].text;
    ck_assert_uint_eq(f[0], ' ');
    ck_assert_uint_eq(f[1], ' ');
    ck_assert_uint_eq(f[2], ' ');
    ck_assert_uint_eq(f[3], 0x22);
    ck_assert(memcmp(f + 4, "GAME", 4) == 0);
    ck_assert_uint_eq(f[8], 0x22);
    for (size_t i = 0; i < 12; i++) {
        ck_assert_uint_eq(f[9 + i], ' ');
    }
    ck_assert_uint_eq(f[21], ' ');
    ck_assert(memcmp(f + 22, "PRG", 3) == 0);
    ck_assert_uint_eq(lines[1].text_len, 25);

    ck_assert_uint_eq(lines[2].linenum, 1);

    // Footer reports the free block count.
    ck_assert_uint_eq(lines[3].linenum, 664);
    ck_assert(memcmp(lines[3].text, "BLOCKS FREE.", 12) == 0);
    ck_assert_uint_eq(lines[3].text_len, 12);
}
END_TEST

// Empty directory: header and footer only.
START_TEST(test_render_empty) {
    uint8_t img[128];
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME, NULL, 0, 100 * 254, true);
    ck_assert_uint_gt(len, 0);

    parsed_line_t lines[4];
    size_t n = parse_lines(img, len, LOAD_ADDR, lines, 4);
    ck_assert_uint_eq(n, 2);    // header + footer

    check_header(&lines[0]);
    ck_assert_uint_eq(lines[1].linenum, 100);
    ck_assert(memcmp(lines[1].text, "BLOCKS FREE.", 12) == 0);
}
END_TEST

// Leading spaces shrink as the block count grows so the quote stays aligned.
START_TEST(test_block_count_alignment) {
    tape_dir_entry_t entries[] = {
        { "A", 5 },         // 1 digit  -> 3 leading spaces
        { "B", 42 },        // 2 digits -> 2 leading spaces
        { "C", 700 },       // 3 digits -> 1 leading space
        { "D", 1234 },      // 4 digits -> 0 leading spaces
    };
    uint8_t img[512];
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME, entries, 4, 0, true);
    ck_assert_uint_gt(len, 0);

    parsed_line_t lines[8];
    size_t n = parse_lines(img, len, LOAD_ADDR, lines, 8);
    ck_assert_uint_eq(n, 6);    // header + 4 files + footer

    const size_t expected_lead[] = { 3, 2, 1, 0 };
    for (size_t i = 0; i < 4; i++) {
        const uint8_t* t = lines[1 + i].text;
        for (size_t s = 0; s < expected_lead[i]; s++) {
            ck_assert_uint_eq(t[s], ' ');
        }
        ck_assert_uint_eq(t[expected_lead[i]], 0x22);   // quote follows lead
    }
}
END_TEST

// Names are uppercased for the PET graphics character set and padded to 16
// columns. In graphics mode the unshifted range $41-$5A displays as uppercase,
// and its bytes equal the ASCII uppercase codes, so the folded letters compare
// equal to "GAME".
START_TEST(test_name_uppercase_and_pad) {
    tape_dir_entry_t entries[] = {
        { "game", 1 },
    };
    uint8_t img[128];
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME, entries, 1, 0, true);
    ck_assert_uint_gt(len, 0);

    parsed_line_t lines[4];
    parse_lines(img, len, LOAD_ADDR, lines, 4);

    const uint8_t* f = lines[1].text;
    ck_assert_uint_eq(f[3], 0x22);
    ck_assert(memcmp(f + 4, "GAME", 4) == 0);   // uppercased
    ck_assert_uint_eq(f[8], 0x22);
    for (size_t i = 0; i < 12; i++) {
        ck_assert_uint_eq(f[9 + i], ' ');       // pad to 16-char field
    }
    ck_assert(memcmp(f + 22, "PRG", 3) == 0);
}
END_TEST

// In text mode the host filename case is preserved: uppercase letters map to the
// shifted PETSCII range ($C1-$DA, which displays as uppercase) and lowercase to
// the unshifted range ($41-$5A, which displays as lowercase), so mixed-case
// names render faithfully.
START_TEST(test_name_text_mode_preserves_case) {
    tape_dir_entry_t entries[] = {
        { "Game", 1 },
    };
    uint8_t img[128];
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME, entries, 1, 0, false);
    ck_assert_uint_gt(len, 0);

    parsed_line_t lines[4];
    parse_lines(img, len, LOAD_ADDR, lines, 4);

    const uint8_t* f = lines[1].text;
    ck_assert_uint_eq(f[3], 0x22);
    ck_assert_uint_eq(f[4], 0xC7);   // 'G' -> shifted (uppercase glyph)
    ck_assert_uint_eq(f[5], 0x41);   // 'a' -> unshifted (lowercase glyph)
    ck_assert_uint_eq(f[6], 0x4D);   // 'm'
    ck_assert_uint_eq(f[7], 0x45);   // 'e'
    ck_assert_uint_eq(f[8], 0x22);
}
END_TEST

// A full 16-character name fills the field with no interior padding.
START_TEST(test_full_length_name) {
    tape_dir_entry_t entries[] = {
        { "ABCDEFGHIJKLMNOP", 9 },
    };
    uint8_t img[128];
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME, entries, 1, 0, true);
    ck_assert_uint_gt(len, 0);

    parsed_line_t lines[4];
    parse_lines(img, len, LOAD_ADDR, lines, 4);

    const uint8_t* f = lines[1].text;
    ck_assert_uint_eq(f[3], 0x22);
    ck_assert(memcmp(f + 4, "ABCDEFGHIJKLMNOP", 16) == 0);
    ck_assert_uint_eq(f[20], 0x22);     // closing quote right after 16 chars
    ck_assert_uint_eq(f[21], ' ');
    ck_assert(memcmp(f + 22, "PRG", 3) == 0);
}
END_TEST

// Insufficient output capacity yields 0 (no partial image).
START_TEST(test_capacity_too_small) {
    tape_dir_entry_t entries[] = {
        { "GAME", 5 },
    };
    uint8_t img[16];    // smaller than even the header line
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME, entries, 1, 0, true);
    ck_assert_uint_eq(len, 0);
}
END_TEST

// Entries with empty names are skipped.
START_TEST(test_skip_empty_names) {
    tape_dir_entry_t entries[] = {
        { "", 1 },
        { "REAL", 2 },
        { "", 3 },
    };
    uint8_t img[256];
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME, entries, 3, 0, true);
    ck_assert_uint_gt(len, 0);

    parsed_line_t lines[8];
    size_t n = parse_lines(img, len, LOAD_ADDR, lines, 8);
    ck_assert_uint_eq(n, 3);    // header + 1 real file + footer
    ck_assert_uint_eq(lines[1].linenum, 2);
    ck_assert(memcmp(lines[1].text + 3, "\"REAL\"", 6) == 0);
}
END_TEST

// Free bytes are reported in 254-byte blocks (the footer line number).
START_TEST(test_free_blocks_conversion) {
    uint8_t img[128];
    // 1000 blocks worth of bytes, plus a partial block that rounds down.
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME, NULL, 0,
                                 1000 * 254 + 253, true);
    ck_assert_uint_gt(len, 0);

    parsed_line_t lines[4];
    size_t n = parse_lines(img, len, LOAD_ADDR, lines, 4);
    ck_assert_uint_eq(n, 2);    // header + footer
    ck_assert_uint_eq(lines[1].linenum, 1000);
}
END_TEST

// Free blocks are clamped to the CBM line number maximum (63999).
START_TEST(test_free_blocks_clamped) {
    uint8_t img[128];
    // Far more than 63999 blocks (4 GiB of free space).
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME, NULL, 0,
                                 (uint64_t)4 * 1024 * 1024 * 1024, true);
    ck_assert_uint_gt(len, 0);

    parsed_line_t lines[4];
    size_t n = parse_lines(img, len, LOAD_ADDR, lines, 4);
    ck_assert_uint_eq(n, 2);    // header + footer
    ck_assert_uint_eq(lines[1].linenum, 63999);
    ck_assert(memcmp(lines[1].text, "BLOCKS FREE.", 12) == 0);
}
END_TEST

// File sizes round up to whole blocks, with a minimum of one block.
START_TEST(test_blocks_from_bytes_round_up) {
    ck_assert_uint_eq(tape_dir_blocks_from_bytes(0, true), 1);      // min 1
    ck_assert_uint_eq(tape_dir_blocks_from_bytes(1, true), 1);
    ck_assert_uint_eq(tape_dir_blocks_from_bytes(254, true), 1);
    ck_assert_uint_eq(tape_dir_blocks_from_bytes(255, true), 2);    // round up
    ck_assert_uint_eq(tape_dir_blocks_from_bytes(508, true), 2);
    // Clamp to the CBM line number maximum (63999 blocks).
    ck_assert_uint_eq(tape_dir_blocks_from_bytes((uint64_t)64000 * 254, true),
                      63999);
}
END_TEST

// Free space rounds down to whole blocks, with zero allowed.
START_TEST(test_blocks_from_bytes_round_down) {
    ck_assert_uint_eq(tape_dir_blocks_from_bytes(0, false), 0);     // zero ok
    ck_assert_uint_eq(tape_dir_blocks_from_bytes(253, false), 0);   // round down
    ck_assert_uint_eq(tape_dir_blocks_from_bytes(254, false), 1);
    ck_assert_uint_eq(tape_dir_blocks_from_bytes(507, false), 1);   // round down
    ck_assert_uint_eq(tape_dir_blocks_from_bytes(508, false), 2);
    // Clamp to the CBM line number maximum (63999 blocks).
    ck_assert_uint_eq(tape_dir_blocks_from_bytes((uint64_t)64000 * 254, false),
                      63999);
}
END_TEST

// ---------------------------------------------------------------------------
// LOAD-by-listing round trip
//
// These tests model the full user workflow: a file's name is shown in the
// synthesized directory listing (host ASCII -> PETSCII via ascii_to_petscii),
// the user reads those glyphs and re-types them to LOAD the file (the PET
// captures the same PETSCII), and the firmware matches that raw PETSCII against
// the host directory entry with cbm_filename_match (which re-encodes the host
// name with the same ascii_to_petscii mapping). The whole chain must round-trip
// so that "what you see is what you can load".
// ---------------------------------------------------------------------------

// Render a one-entry listing for host base name 'base' in the given charset
// mode, then read the raw PETSCII bytes shown between the quotes on the file
// line (exactly what the user sees and types). Writes the resulting LOAD
// pattern to 'pattern_out' and returns its length.
static uint8_t listing_roundtrip_pattern(const char* base, bool graphics_mode,
                                         uint8_t* pattern_out) {
    tape_dir_entry_t entries[1];
    memset(entries, 0, sizeof(entries));
    strncpy(entries[0].name, base, TAPE_DIR_MAX_NAME);
    entries[0].name[TAPE_DIR_MAX_NAME] = '\0';
    entries[0].blocks = 1;

    uint8_t img[256];
    size_t len = tape_dir_render(img, sizeof(img), LOAD_ADDR, DISK_NAME,
                                 entries, 1, 0, graphics_mode);
    ck_assert_uint_gt(len, 0);

    parsed_line_t lines[4];
    size_t n = parse_lines(img, len, LOAD_ADDR, lines, 4);
    ck_assert_uint_eq(n, 3);    // header + file + footer

    const uint8_t* t = lines[1].text;
    size_t i = 0;
    while (t[i] != PETSCII_QUOTE) i++;      // skip leading spaces to opening quote
    i++;                                    // step past the opening quote

    uint8_t plen = 0;
    while (t[i] != PETSCII_QUOTE) {         // read up to the closing quote
        pattern_out[plen++] = t[i];
        i++;
    }
    pattern_out[plen] = '\0';
    return plen;
}

// Assert that "<base>.prg" loads when the user types the name shown in the
// listing, in both graphics (folded) and text (case-preserving) charset modes.
static void check_roundtrip_matches(const char* base) {
    char filename[64];
    snprintf(filename, sizeof(filename), "%s.prg", base);

    uint8_t pattern[TAPE_DIR_MAX_NAME + 1];
    for (int gfx = 0; gfx <= 1; gfx++) {
        uint8_t plen = listing_roundtrip_pattern(base, gfx != 0, pattern);
        ck_assert_msg(cbm_filename_match(pattern, plen, filename),
                      "no match: base=\"%s\" gfx=%d", base, gfx);
    }
}

// Letters always round-trip regardless of case or charset mode: graphics mode
// folds to the uppercase-displaying range and text mode preserves case, and
// cbm_filename_match folds both sides, so the file is found either way.
START_TEST(test_roundtrip_casing) {
    check_roundtrip_matches("GAME");       // all uppercase
    check_roundtrip_matches("game");       // all lowercase
    check_roundtrip_matches("Game");       // mixed
    check_roundtrip_matches("GaMe");       // alternating
    check_roundtrip_matches("A");          // single letter
    check_roundtrip_matches("Z");
}
END_TEST

// Digits and embedded spaces are identity in PETSCII and survive the round trip.
START_TEST(test_roundtrip_digits_and_spaces) {
    check_roundtrip_matches("1942");
    check_roundtrip_matches("DEMO 2");
    check_roundtrip_matches("MY GAME");
    check_roundtrip_matches("LEVEL 10");
    check_roundtrip_matches("3D MAZE");
}
END_TEST

// Punctuation that shares its code point between ASCII and PETSCII (and that
// petscii_to_ascii leaves untouched) round-trips exactly.
START_TEST(test_roundtrip_identity_punctuation) {
    check_roundtrip_matches("GAME-2");
    check_roundtrip_matches("DOC.V2");
    check_roundtrip_matches("A+B");
    check_roundtrip_matches("(DEMO)");
    check_roundtrip_matches("R&D");
    check_roundtrip_matches("PART#1");
    check_roundtrip_matches("X=Y");
    check_roundtrip_matches("[CORE]");
    check_roundtrip_matches("@HOME");
    check_roundtrip_matches("100%");
    check_roundtrip_matches("HI!");
}
END_TEST

// A full 16-character name (the field width) round-trips with no truncation,
// in either case and including spaces and identity punctuation.
START_TEST(test_roundtrip_full_length_name) {
    check_roundtrip_matches("ABCDEFGHIJKLMNOP");   // exactly 16 chars
    check_roundtrip_matches("abcdefghijklmnop");
    check_roundtrip_matches("Mixed Case Name!");   // 16 chars, spaces + punct
}
END_TEST

// The user can also type a prefix of the displayed name plus '*' to load it.
START_TEST(test_roundtrip_wildcard_prefix) {
    uint8_t pattern[TAPE_DIR_MAX_NAME + 1];

    for (int gfx = 0; gfx <= 1; gfx++) {
        uint8_t full = listing_roundtrip_pattern("ADVENTURE", gfx != 0, pattern);
        ck_assert_uint_eq(full, 9);

        // Truncate to a 3-char prefix and append the wildcard.
        pattern[3] = '*';
        pattern[4] = '\0';
        ck_assert(cbm_filename_match(pattern, 4, "adventure.prg"));
    }
}
END_TEST

// Encode an ASCII LOAD pattern to PETSCII (as the PET captures it) and match it
// against a host filename, exercising cbm_filename_match the way the firmware
// calls it.
static bool match_ascii(const char* ascii_pattern, const char* filename) {
    uint8_t petscii[TAPE_DIR_MAX_NAME + 1];
    uint8_t len = 0;
    for (const char* p = ascii_pattern; *p != '\0'; p++) {
        petscii[len++] = ascii_to_petscii((uint8_t)*p, /* graphics_mode: */ false);
    }
    return cbm_filename_match(petscii, len, filename);
}

// An exact (non-wildcard) pattern matches "pattern" or "pattern.prg" only. It
// must not match a file with an embedded dot such as "foo.bar.prg".
START_TEST(test_exact_match_requires_prg_extension) {
    ck_assert(match_ascii("foo", "foo"));
    ck_assert(match_ascii("foo", "foo.prg"));
    ck_assert(match_ascii("foo", "FOO.PRG"));
    ck_assert(!match_ascii("foo", "foo.bar.prg"));
    ck_assert(!match_ascii("foo", "foo.txt"));
    ck_assert(!match_ascii("foo", "foobar"));

    // The .prg extension match is case-insensitive.
    ck_assert(match_ascii("a", "a.PRG"));
    ck_assert(match_ascii("a", "a.Prg"));

    // A trailing wildcard still matches embedded dots ("a*" vs "a.b.prg").
    ck_assert(match_ascii("a*", "a.b.prg"));
}
END_TEST


// An empty pattern and a lone "*" must never match. The firmware relies on these
// returning false so a bare LOAD (no name) and LOAD "*" fall through to the
// physical datasette instead of grabbing the first directory entry.
START_TEST(test_empty_and_bare_wildcard_never_match) {
    // Empty pattern (pattern_len == 0): pointer contents are irrelevant.
    ck_assert(!cbm_filename_match((const uint8_t*)"", 0, "foo.prg"));
    ck_assert(!cbm_filename_match((const uint8_t*)"foo", 0, "foo.prg"));

    // Lone "*" wildcard.
    ck_assert(!match_ascii("*", "foo.prg"));
    ck_assert(!match_ascii("*", "anything"));

    // A non-empty prefix before the wildcard still matches normally.
    ck_assert(match_ascii("f*", "foo.prg"));
}
END_TEST


// Punctuation that cc65's ascii_to_petscii relocates ($5C '\', $5F '_',
// $60 '`', $7B '{', $7C '|', $7D '}', $7E '~') previously failed to round-trip:
// the firmware decoded the typed name with the letter-only petscii_to_ascii and
// lost these glyphs, so a listed file could not be loaded. Matching now happens
// in PETSCII space (the host name is re-encoded with the same ascii_to_petscii
// the listing renders with), so these names match exactly. Underscore in
// particular is a very common host filename character.
START_TEST(test_roundtrip_remapped_punctuation) {
    static const char* const bases[] = {
        "A_B",      // underscore -> $A4
        "A\\B",     // backslash  -> $BF
        "A~B",      // tilde      -> $B1
        "A`B",      // backtick   -> $AD
        "A{B}",     // braces     -> $B3 / $AB
        "A|B",      // bar        -> $DD
    };

    char filename[64];
    uint8_t pattern[TAPE_DIR_MAX_NAME + 1];

    for (size_t k = 0; k < sizeof(bases) / sizeof(bases[0]); k++) {
        snprintf(filename, sizeof(filename), "%s.prg", bases[k]);
        for (int gfx = 0; gfx <= 1; gfx++) {
            uint8_t plen = listing_roundtrip_pattern(bases[k], gfx != 0, pattern);
            ck_assert_msg(cbm_filename_match(pattern, plen, filename),
                          "no match: base=\"%s\" gfx=%d", bases[k], gfx);
        }
    }
}
END_TEST

Suite *tape_dir_suite(void) {
    Suite* s = suite_create("tape_dir");
    TCase* tc = tcase_create("render");

    tcase_add_test(tc, test_render_basic);
    tcase_add_test(tc, test_render_empty);
    tcase_add_test(tc, test_block_count_alignment);
    tcase_add_test(tc, test_name_uppercase_and_pad);
    tcase_add_test(tc, test_name_text_mode_preserves_case);
    tcase_add_test(tc, test_full_length_name);
    tcase_add_test(tc, test_capacity_too_small);
    tcase_add_test(tc, test_skip_empty_names);
    tcase_add_test(tc, test_free_blocks_conversion);
    tcase_add_test(tc, test_free_blocks_clamped);
    tcase_add_test(tc, test_blocks_from_bytes_round_up);
    tcase_add_test(tc, test_blocks_from_bytes_round_down);
    tcase_add_test(tc, test_roundtrip_casing);
    tcase_add_test(tc, test_roundtrip_digits_and_spaces);
    tcase_add_test(tc, test_roundtrip_identity_punctuation);
    tcase_add_test(tc, test_roundtrip_full_length_name);
    tcase_add_test(tc, test_roundtrip_wildcard_prefix);
    tcase_add_test(tc, test_exact_match_requires_prg_extension);
    tcase_add_test(tc, test_empty_and_bare_wildcard_never_match);
    tcase_add_test(tc, test_roundtrip_remapped_punctuation);

    suite_add_tcase(s, tc);
    return s;
}
