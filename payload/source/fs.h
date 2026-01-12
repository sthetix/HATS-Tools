/*
 * HATS Installer - Filesystem operations
 */

#pragma once
#include <utils/types.h>

// Error code to string
const char *fs_error_str(int err);

// File/folder operations - returns 0 on success
int file_copy(const char *src, const char *dst);
int folder_copy(const char *src, const char *dst);
int folder_delete(const char *path);

// File logging
void log_init(const char *path);
void log_close(void);
void log_write(const char *fmt, ...);
