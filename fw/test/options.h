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
    OptionKind_Action,
    OptionKind_End,
} OptionKind;

typedef struct {
    const char** const values;
    int count;
    int selected;
} OptionSelect;

typedef struct {
    const char* const label;
} OptionAction;

typedef struct {
    OptionKind kind;
    union {
        OptionSelect select;
        OptionAction action;
        const char END;
    };
} Option;

typedef struct {
    const char* name;
    Option* options;
} Group;

typedef struct {
    Option* pOption;
    uint8_t value;
    uint8_t x;
    uint8_t y;
    uint8_t w;
} Layout;

Layout layouts[32] = { 0 };

// Index of groups.
typedef enum {
    Group_Basic = 0,
    Group_Video = 1,
    Group_Button = 2
} GroupIndex;

// Index of option within group.
typedef enum {
    Option_Basic_Version = 0,
    Option_Video_Columns = 0,
    Option_Video_VSync = 2,
    Option_Button_Reset = 0
} OptionIndex;

typedef enum {
    Option_Basic_Version_2 = 0,
    Option_Basic_Version_3 = 1,
    Option_Basic_Version_4 = 2,
    Option_Video_Columns_40 = 0,
    Option_Video_Columns_80 = 1,
    Option_Video_VSync_50Hz = 0,
    Option_Video_VSync_60Hz = 1
} OptionValueIndices;

typedef enum {
    HZ_50 = 0,
    HZ_60 = 1
} VSync;

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
    {
        .name = "Action",
        .options = (Option[]) {
            {
                .kind = OptionKind_Action,
                .action = {
                    .label = "Reset"
                }
            },
        },
    },
};
