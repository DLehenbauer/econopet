// SPDX-License-Identifier: CC0-1.0
// https://github.com/dlehenbauer/econopet

#pragma once

// Use pico-vfs's directory entries rather than glibc's incompatible layout.
typedef struct sd_diag_mock_dir DIR;
struct dirent;

/** Open a directory on the fake mounted SD filesystem. */
DIR* opendir(const char* path);

/** Read a pico-vfs directory entry from the fake filesystem. */
struct dirent* readdir(DIR* directory);

/** Close a directory owned by the fake filesystem. */
int closedir(DIR* directory);
