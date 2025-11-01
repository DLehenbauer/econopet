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

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "system_state.h"
#include "video/video.h"
#include "sd/sd.h"
#include "mock.h"

uint8_t video_char_buffer[VIDEO_CHAR_BUFFER_BYTE_SIZE];
bool video_graphics = false;
bool video_is_80_col = false;

// In-memory file system for testing
typedef struct mem_file_s {
    char path[PATH_MAX];
    char* content;
    size_t size;
    size_t capacity;
    struct mem_file_s* next;
} mem_file_t;

static mem_file_t* mem_files = NULL;

// Find an in-memory file
static mem_file_t* find_mem_file(const char* path) {
    for (mem_file_t* file = mem_files; file != NULL; file = file->next) {
        if (strcmp(file->path, path) == 0) {
            return file;
        }
    }
    return NULL;
}

// Register an in-memory file for testing
void mock_register_file(const char* path, const char* content) {
    assert(path[0] == '/');
    
    // Caller must unregister an existing file before re-registering it.
    assert(find_mem_file(path) == NULL);
    
    mem_file_t* file = malloc(sizeof(mem_file_t));
    strncpy(file->path, path, sizeof(file->path) - 1);
    file->path[sizeof(file->path) - 1] = '\0';
    
    file->size = strlen(content);
    file->capacity = file->size + 1;
    file->content = malloc(file->capacity);
    memcpy(file->content, content, file->size + 1);
    
    file->next = mem_files;
    mem_files = file;
}

// Unregister an in-memory file
void mock_unregister_file(const char* path) {
    mem_file_t** pp = &mem_files;
    while (*pp) {
        if (strcmp((*pp)->path, path) == 0) {
            mem_file_t* to_free = *pp;
            *pp = to_free->next;
            free(to_free->content);
            free(to_free);
            return;
        }
        pp = &(*pp)->next;
    }
}

// Clear all registered in-memory files
void mock_clear_files(void) {
    while (mem_files) {
        mem_file_t* next = mem_files->next;
        free(mem_files->content);
        free(mem_files);
        mem_files = next;
    }
}

// Mock implementation of 'sd_open' returns contents of previously registered in-memory files
// using 'mock_register_file()'.
FILE* sd_open(const char* path, const char* mode) {
    assert(path[0] == '/');
    
    // Check if this is an in-memory file
    mem_file_t* mem_file = find_mem_file(path);
    if (mem_file != NULL) {
        // For in-memory files, only support read mode
        if (strcmp(mode, "r") == 0) {
            return fmemopen(mem_file->content, mem_file->size, "r");
        }
        // If write mode requested for in-memory file, fail
        fprintf(stderr, "FATAL: Write mode not supported for in-memory file '%s'\n", path);
        exit(1);
    }
    
    // File not registered - fail with clear error
    fprintf(stderr, "FATAL: File '%s' not registered in mock file system.\n", path);
    fprintf(stderr, "Use mock_register_file() to register files for testing.\n");
    exit(1);
}

void term_present() {
    fflush(stdout);
}

void set_cpu(bool ready, bool reset, bool nmi) {
    (void)ready;
    (void)reset;
    (void)nmi;
}

void start_menu_rom() { }

void spi_fill(uint32_t addr, uint8_t byte, size_t byteLength) {
    (void)addr;
    (void)byte;
    (void)byteLength;
}

void test_ram() { }
