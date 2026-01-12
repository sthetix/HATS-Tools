/*
 * HATS Installer - Filesystem operations with file logging
 */

#include "fs.h"
#include <libs/fatfs/ff.h>
#include <mem/heap.h>
#include <string.h>
#include <utils/sprintf.h>
#include <stdarg.h>
#include <stdio.h>

#define FS_BUFFER_SIZE 0x100000  // 1MB copy buffer
#define LOG_BUFFER_SIZE 512

// Log file handle
static FIL log_file;
static bool log_enabled = false;
static char log_buf[LOG_BUFFER_SIZE];

// Initialize log file
void log_init(const char *path) {
    int res = f_open(&log_file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res == FR_OK) {
        log_enabled = true;
        log_write("=== HATS Installer Log ===\n\n");
    }
}

// Close log file
void log_close(void) {
    if (log_enabled) {
        f_sync(&log_file);
        f_close(&log_file);
        log_enabled = false;
    }
}

// Write to log file (variadic version)
void log_write(const char *fmt, ...) {
    if (!log_enabled) return;

    va_list args;
    va_start(args, fmt);

    // Format the message
    vsnprintf(log_buf, LOG_BUFFER_SIZE, fmt, args);

    va_end(args);

    // Write to file
    UINT bw;
    f_write(&log_file, log_buf, strlen(log_buf), &bw);
    f_sync(&log_file);  // Flush to ensure data is written
}

// Convert FatFS error code to string
const char *fs_error_str(int err) {
    switch (err) {
        case FR_OK:                  return "OK";
        case FR_DISK_ERR:            return "DISK_ERR: Low level disk error";
        case FR_INT_ERR:             return "INT_ERR: Internal error";
        case FR_NOT_READY:           return "NOT_READY: Drive not ready";
        case FR_NO_FILE:             return "NO_FILE: File not found";
        case FR_NO_PATH:             return "NO_PATH: Path not found";
        case FR_INVALID_NAME:        return "INVALID_NAME: Invalid path name";
        case FR_DENIED:              return "DENIED: Access denied";
        case FR_EXIST:               return "EXIST: Already exists";
        case FR_INVALID_OBJECT:      return "INVALID_OBJECT: Invalid object";
        case FR_WRITE_PROTECTED:     return "WRITE_PROTECTED: Write protected";
        case FR_INVALID_DRIVE:       return "INVALID_DRIVE: Invalid drive";
        case FR_NOT_ENABLED:         return "NOT_ENABLED: Volume not mounted";
        case FR_NO_FILESYSTEM:       return "NO_FILESYSTEM: No valid FAT";
        case FR_MKFS_ABORTED:        return "MKFS_ABORTED: mkfs aborted";
        case FR_TIMEOUT:             return "TIMEOUT: Timeout";
        case FR_LOCKED:              return "LOCKED: File locked";
        case FR_NOT_ENOUGH_CORE:     return "NOT_ENOUGH_CORE: Out of memory";
        case FR_TOO_MANY_OPEN_FILES: return "TOO_MANY_OPEN_FILES";
        case FR_INVALID_PARAMETER:   return "INVALID_PARAMETER";
        default:                     return "UNKNOWN_ERROR";
    }
}

// Combine two paths
static char *combine_paths(const char *base, const char *add) {
    size_t base_len = strlen(base);
    size_t add_len = strlen(add);
    char *result = malloc(base_len + add_len + 2);

    if (!result) return NULL;

    if (base_len > 0 && base[base_len - 1] == '/') {
        s_printf(result, "%s%s", base, add);
    } else {
        s_printf(result, "%s/%s", base, add);
    }

    return result;
}

// Copy a single file with logging
int file_copy(const char *src, const char *dst) {
    FIL fin, fout;
    FILINFO fno;
    int res;

    log_write("COPY: %s -> %s\n", src, dst);

    res = f_open(&fin, src, FA_READ | FA_OPEN_EXISTING);
    if (res != FR_OK) {
        log_write("  ERROR open src: %s\n", fs_error_str(res));
        return res;
    }

    f_stat(src, &fno);
    u64 file_size = f_size(&fin);
    log_write("  Size: %d bytes\n", (u32)file_size);

    res = f_open(&fout, dst, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        f_close(&fin);
        log_write("  ERROR open dst: %s\n", fs_error_str(res));
        return res;
    }

    u8 *buf = malloc(FS_BUFFER_SIZE);
    if (!buf) {
        f_close(&fin);
        f_close(&fout);
        log_write("  ERROR: Out of memory for buffer\n");
        return FR_NOT_ENOUGH_CORE;
    }

    u64 remaining = file_size;
    UINT br, bw;

    while (remaining > 0) {
        UINT to_copy = (remaining > FS_BUFFER_SIZE) ? FS_BUFFER_SIZE : (UINT)remaining;

        res = f_read(&fin, buf, to_copy, &br);
        if (res != FR_OK) {
            log_write("  ERROR read: %s\n", fs_error_str(res));
            break;
        }
        if (br != to_copy) {
            log_write("  ERROR: Read %d bytes, expected %d\n", br, to_copy);
            res = FR_DISK_ERR;
            break;
        }

        res = f_write(&fout, buf, to_copy, &bw);
        if (res != FR_OK) {
            log_write("  ERROR write: %s\n", fs_error_str(res));
            break;
        }
        if (bw != to_copy) {
            log_write("  ERROR: Wrote %d bytes, expected %d\n", bw, to_copy);
            res = FR_DISK_ERR;
            break;
        }

        remaining -= to_copy;
    }

    free(buf);
    f_close(&fin);
    f_close(&fout);

    if (res == FR_OK) {
        f_chmod(dst, fno.fattrib, 0x3A);
        log_write("  OK\n");
    }

    return res;
}

// Recursively delete a folder with logging
int folder_delete(const char *path) {
    DIR dir;
    FILINFO fno;
    int res;

    log_write("DELETE: %s\n", path);

    res = f_opendir(&dir, path);
    if (res != FR_OK) {
        // Maybe it's a file, try to delete it
        log_write("  Not a dir, trying as file...\n");
        res = f_unlink(path);
        if (res != FR_OK) {
            log_write("  ERROR unlink: %s\n", fs_error_str(res));
        } else {
            log_write("  OK (file deleted)\n");
        }
        return res;
    }

    int file_count = 0;
    int dir_count = 0;

    while (1) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK) {
            log_write("  ERROR readdir: %s\n", fs_error_str(res));
            break;
        }
        if (fno.fname[0] == 0) break;  // End of directory

        char *full_path = combine_paths(path, fno.fname);
        if (!full_path) {
            res = FR_NOT_ENOUGH_CORE;
            break;
        }

        if (fno.fattrib & AM_DIR) {
            dir_count++;
            res = folder_delete(full_path);
        } else {
            file_count++;
            log_write("  DEL: %s\n", fno.fname);
            res = f_unlink(full_path);
            if (res != FR_OK) {
                log_write("    ERROR: %s\n", fs_error_str(res));
            }
        }

        free(full_path);
        if (res != FR_OK) break;
    }

    f_closedir(&dir);

    if (res == FR_OK || res == FR_NO_FILE) {
        log_write("  Removing dir: %s (%d files, %d subdirs)\n", path, file_count, dir_count);
        res = f_unlink(path);
        if (res != FR_OK) {
            log_write("  ERROR rmdir: %s\n", fs_error_str(res));
        } else {
            log_write("  OK\n");
        }
    }

    return res;
}

// Recursively copy a folder with logging
int folder_copy(const char *src, const char *dst) {
    DIR dir;
    FILINFO fno;
    int res;

    log_write("FOLDER COPY: %s -> %s\n", src, dst);

    res = f_opendir(&dir, src);
    if (res != FR_OK) {
        log_write("  ERROR opendir src: %s\n", fs_error_str(res));
        return res;
    }

    // Get folder name from src path
    const char *folder_name = strrchr(src, '/');
    if (folder_name) {
        folder_name++;
    } else {
        folder_name = src;
    }

    // Create destination folder
    char *dst_path = combine_paths(dst, folder_name);
    if (!dst_path) {
        f_closedir(&dir);
        return FR_NOT_ENOUGH_CORE;
    }

    log_write("  Creating: %s\n", dst_path);

    res = f_mkdir(dst_path);
    if (res == FR_EXIST) {
        log_write("  (already exists)\n");
        res = FR_OK;
    } else if (res != FR_OK) {
        log_write("  ERROR mkdir: %s\n", fs_error_str(res));
        f_closedir(&dir);
        free(dst_path);
        return res;
    }

    int file_count = 0;
    int dir_count = 0;

    // Copy contents
    while (1) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK) {
            log_write("  ERROR readdir: %s\n", fs_error_str(res));
            break;
        }
        if (fno.fname[0] == 0) break;  // End of directory

        char *src_full = combine_paths(src, fno.fname);
        char *dst_full = combine_paths(dst_path, fno.fname);

        if (!src_full || !dst_full) {
            if (src_full) free(src_full);
            if (dst_full) free(dst_full);
            res = FR_NOT_ENOUGH_CORE;
            break;
        }

        if (fno.fattrib & AM_DIR) {
            dir_count++;
            res = folder_copy(src_full, dst_path);
        } else {
            file_count++;
            res = file_copy(src_full, dst_full);
        }

        free(src_full);
        free(dst_full);

        if (res != FR_OK) break;
    }

    f_closedir(&dir);

    // Copy folder attributes
    if (res == FR_OK) {
        FILINFO src_info;
        if (f_stat(src, &src_info) == FR_OK) {
            f_chmod(dst_path, src_info.fattrib, 0x3A);
        }
        log_write("  Done: %d files, %d subdirs\n", file_count, dir_count);
    }

    free(dst_path);
    return res;
}
