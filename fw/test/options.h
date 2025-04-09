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

#pragma once

#include <stdint.h>

typedef enum {
    OptionKind_Choice,
    OptionKind_Boolean,
    OptionKind_Spacer,
    OptionKind_End
} OptionKind;

typedef struct {
    const char** const values;
    int count;
    int selected;
} OptionSelect;

typedef struct {
    OptionKind kind;
    union {
        OptionSelect select;
        struct {
            const char* name;
            bool enabled;
        } boolean;
        const char END;
    };
} Option;

typedef struct {
    const char* name;
    Option* options;
} Group;

typedef enum {
    Option_BasicVersion = 0,
    Option_VSync = 1
} OptionIndex;

typedef enum {
    Version_2 = 0,
    Version_3 = 1,
    Version_4 = 2
} BasicVersion;

typedef enum {
    HZ_50 = 0,
    HZ_60 = 1
} VSync;

typedef struct {
    Option* pOption;
    uint8_t value;
    uint8_t x;
    uint8_t y;
    uint8_t w;
} Layout;

Layout layouts[32] = { 0 };

Group groups[] = {
    {
        .name = "Basic",
        .options = (Option[]) {
            {
                .kind = OptionKind_Choice,
                .select = {
                    .values = (const char*[]) { "Basic 2.0", "Basic 3.0", "Basic 4.0" },
                    .count = 3,
                    .selected = 2
                }
            },
            { .kind = OptionKind_End, .END = 0 }
        },
    },
    {
        .name = "Video",
        .options = (Option[]) {
            {
                .kind = OptionKind_Choice,
                .select = {
                    .values = (const char*[]) { "40 Columns", "80 Columns" },
                    .count = 2,
                    .selected = 1
                }
            },
            { .kind = OptionKind_Spacer, .END = 0 },
            {
                .kind = OptionKind_Choice,
                .select = {
                    .values = (const char*[]) { "50 Hz", "60 Hz" },
                    .count = 2,
                    .selected = 1
                }
            },
            { .kind = OptionKind_End, .END = 0 }
        },
    },
};
