// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#include "pch.h"
#include "sd.h"

#include "ff.h"

_Static_assert(FF_CODE_PAGE == 932 && FF_LFN_UNICODE == 0,
               "SD path validation requires the configured CP932 byte encoding");

#define CP932_LEAD_LOW_FIRST 0x81u
#define CP932_LEAD_LOW_LAST 0x9fu
#define CP932_LEAD_HIGH_FIRST 0xe0u
#define CP932_LEAD_HIGH_LAST 0xfcu
#define CP932_TRAIL_LOW_FIRST 0x40u
#define CP932_TRAIL_LOW_LAST 0x7eu
#define CP932_TRAIL_HIGH_FIRST 0x80u
#define CP932_TRAIL_HIGH_LAST 0xfcu
#define ASCII_DELETE 0x7fu

// Resolve each directory as well as the leaf, since either can use a short-name alias.
bool sd_resolve_path(char path[SD_PATH_MAX]) {
    char resolved[SD_PATH_MAX] = "";
    size_t used = 0;
    for (char* component = path; *component != '\0';) {
        if (*component++ != '/') {
            return false;
        }
        char* end = component;
        while (*end != '\0' && *end != '/') {
            const uint8_t byte = (uint8_t)*end++;
            if ((byte >= CP932_LEAD_LOW_FIRST && byte <= CP932_LEAD_LOW_LAST) ||
                (byte >= CP932_LEAD_HIGH_FIRST && byte <= CP932_LEAD_HIGH_LAST)) {
                // CP932 trail bytes can be backslashes, but never forward slashes.
                const uint8_t trail = (uint8_t)*end;
                if (!((trail >= CP932_TRAIL_LOW_FIRST && trail <= CP932_TRAIL_LOW_LAST) ||
                      (trail >= CP932_TRAIL_HIGH_FIRST && trail <= CP932_TRAIL_HIGH_LAST))) {
                    return false;
                }
                ++end;
            } else if (byte == '\\' || byte < ' ' || byte == ASCII_DELETE) {
                return false;
            }
        }
        const char separator = *end;
        *end = '\0';
        FILINFO info;
        const FRESULT result = f_stat(path, &info);
        *end = separator;
        if (result != FR_OK || (separator != '\0' && !(info.fattrib & AM_DIR))) {
            return false;
        }
        // f_stat on an SFN alias can return only the SFN. Enumeration recovers its LFN.
        FATFS_DIR directory;
        if (f_opendir(&directory, used == 0 ? "/" : resolved) != FR_OK) {
            return false;
        }
        FILINFO entry;
        bool found = false;
        const char* const identity = info.altname[0] != '\0' ? info.altname : info.fname;
        while (f_readdir(&directory, &entry) == FR_OK && entry.fname[0] != '\0') {
            const char* const candidate = entry.altname[0] != '\0' ? entry.altname : entry.fname;
            if (strcmp(identity, candidate) == 0) {
                info = entry;
                found = true;
                break;
            }
        }
        const FRESULT closed = f_closedir(&directory);
        if (!found || closed != FR_OK) {
            return false;
        }
        const size_t length = strlen(info.fname);
        if (length == 0 || used + 1 + length >= sizeof(resolved)) {
            return false;
        }
        resolved[used++] = '/';
        memcpy(resolved + used, info.fname, length + 1);
        used += length;
        component = end;
    }
    strcpy(path, resolved);
    return true;
}
