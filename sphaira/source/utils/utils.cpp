#include "utils/utils.hpp"
#include "log.hpp"
#include "fs.hpp"

#include <cstring>
#include <cstdio>
#include <switch.h>
#include <vector>

namespace sphaira::utils {
namespace {

// IRAM payload constants (from reboot_to_payload in ftpsrv)
constexpr u32 IRAM_PAYLOAD_MAX_SIZE = 0x24000;
constexpr uintptr_t AMS_IWRAM_OFFSET = 0x40010000;

HashStr hexIdToStrInternal(auto id) {
    HashStr str{};
    const auto id_lower = std::byteswap(*(u64*)id.c);
    const auto id_upper = std::byteswap(*(u64*)(id.c + 0x8));
    std::snprintf(str.str, 0x21, "%016lx%016lx", id_lower, id_upper);
    return str;
}

std::string formatSizeInetrnal(double size, double base) {
    static const char* const suffixes[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB" };
    size_t suffix_index = 0;

    while (size >= base && suffix_index < std::size(suffixes) - 1) {
        size /= base;
        suffix_index++;
    }

    char buffer[32];
    if (suffix_index == 0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f %s", size, suffixes[suffix_index]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", size, suffixes[suffix_index]);
    }

    return buffer;
}

} // namespace

HashStr hexIdToStr(FsRightsId id) {
    return hexIdToStrInternal(id);
}

HashStr hexIdToStr(NcmRightsId id) {
    return hexIdToStrInternal(id.rights_id);
}

HashStr hexIdToStr(NcmContentId id) {
    return hexIdToStrInternal(id);
}

std::string formatSizeStorage(u64 size) {
    return formatSizeInetrnal(size, 1024.0);
}

std::string formatSizeNetwork(u64 size) {
    return formatSizeInetrnal(size, 1000.0);
}

// Hekate IPL ini manipulation
namespace {
    constexpr const char* HEKATE_INI_PATH = "/bootloader/hekate_ipl.ini";
    constexpr const char* HEKATE_INI_BAK_PATH = "/bootloader/hekate_ipl.ini.bak";

    // Find the position of a key in a section, returns nullptr if not found
    const char* findKeyInSection(const char* content, const char* sectionStart, const char* key) {
        const char* p = sectionStart;
        while (*p && *p != '[') {  // Stop at next section
            if (strncmp(p, key, strlen(key)) == 0 && p[strlen(key)] == '=') {
                return p;
            }
            // Move to next line
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
        return nullptr;
    }
}

// Set hekate_ipl.ini to auto-boot HATS installer payload
bool setHekateAutobootPayload(const char* payload_path) {
    log_write("setHekateAutobootPayload: setting up autoboot for %s\n", payload_path);

    // Step 1: Read current hekate_ipl.ini
    FILE* f_ini = fopen(HEKATE_INI_PATH, "rb");
    std::string ini_content;

    if (f_ini) {
        fseek(f_ini, 0, SEEK_END);
        long size = ftell(f_ini);
        fseek(f_ini, 0, SEEK_SET);

        if (size > 0) {
            ini_content.resize(size);
            fread(ini_content.data(), 1, size, f_ini);
        }
        fclose(f_ini);
    } else {
        log_write("setHekateAutobootPayload: hekate_ipl.ini not found, creating new\n");
    }

    // Step 2: Backup original file
    FILE* f_bak = fopen(HEKATE_INI_BAK_PATH, "wb");
    if (!f_bak) {
        log_write("setHekateAutobootPayload: failed to create backup\n");
        return false;
    }

    if (!ini_content.empty()) {
        fwrite(ini_content.data(), 1, ini_content.size(), f_bak);
    } else {
        // Write empty backup if original didn't exist
        const char* empty = "";
        fwrite(empty, 1, 0, f_bak);
    }
    fclose(f_bak);
    log_write("setHekateAutobootPayload: backed up to %s\n", HEKATE_INI_BAK_PATH);

    // Step 3: Build new ini content
    std::string new_ini;

    // Find or create [config] section
    const char* config_section = strstr(ini_content.c_str(), "[config]");
    size_t config_end = 0;

    if (config_section) {
        // Find end of [config] section (next '[' or end of file)
        const char* p = config_section + 8; // Skip "[config]"
        while (*p && *p != '[') {
            if (*p == '\n') {
                const char* next_line = p + 1;
                if (*next_line == '[' || *next_line == '\0') {
                    break;
                }
            }
            p++;
        }
        config_end = p - ini_content.c_str();

        // Copy everything up to and including [config] section
        new_ini.append(ini_content.c_str(), config_end);
    } else {
        // No [config] section, create one
        new_ini = "[config]\n";
    }

    // Step 4: Add/update autoboot settings
    // Check if autoboot key exists and update it, otherwise append
    const char* autoboot_pos = config_section ?
        findKeyInSection(new_ini.c_str(), new_ini.c_str(), "autoboot") : nullptr;

    if (autoboot_pos) {
        // Replace existing autoboot value
        std::string modified = new_ini;
        size_t pos = autoboot_pos - new_ini.c_str();
        size_t line_start = pos;

        // Find start of line (or section start)
        while (line_start > 0 && modified[line_start - 1] != '\n') {
            line_start--;
        }

        // Find end of line
        size_t line_end = pos;
        while (line_end < modified.size() && modified[line_end] != '\n') {
            line_end++;
        }

        // Replace the line
        modified.replace(line_start, line_end - line_start, "autoboot=1\n");
        new_ini = modified;
    } else {
        // Append autoboot setting
        new_ini += "autoboot=1\n";
    }

    // Similar for bootwait
    const char* bootwait_pos = config_section ?
        findKeyInSection(new_ini.c_str(), new_ini.c_str(), "bootwait") : nullptr;

    if (bootwait_pos) {
        std::string modified = new_ini;
        size_t pos = bootwait_pos - new_ini.c_str();
        size_t line_start = pos;
        while (line_start > 0 && modified[line_start - 1] != '\n') {
            line_start--;
        }
        size_t line_end = pos;
        while (line_end < modified.size() && modified[line_end] != '\n') {
            line_end++;
        }
        modified.replace(line_start, line_end - line_start, "bootwait=0\n");
        new_ini = modified;
    } else {
        new_ini += "bootwait=0\n";
    }

    // Step 5: Add HATS Installer section as first entry (right after [config])
    new_ini += "\n[HATS Installer]\n";
    new_ini += "payload=";
    new_ini += payload_path;
    new_ini += "\n";

    // Step 6: Append the rest of the original ini (after [config] section)
    if (config_section && config_end > 0) {
        const char* rest = ini_content.c_str() + config_end;
        // Skip leading newlines
        while (*rest == '\n') rest++;
        new_ini += "\n";
        new_ini += rest;
    }

    // Step 7: Write new ini file
    FILE* f_out = fopen(HEKATE_INI_PATH, "wb");
    if (!f_out) {
        log_write("setHekateAutobootPayload: failed to open %s for writing\n", HEKATE_INI_PATH);
        return false;
    }

    fwrite(new_ini.data(), 1, new_ini.size(), f_out);
    fclose(f_out);

    fsdevCommitDevice("sdmc");

    log_write("setHekateAutobootPayload: hekate_ipl.ini updated successfully\n");
    return true;
}

// Restore hekate_ipl.ini from backup
bool restoreHekateIni() {
    FILE* f_bak = fopen(HEKATE_INI_BAK_PATH, "rb");
    if (!f_bak) {
        log_write("restoreHekateIni: no backup found, nothing to restore\n");
        return false;
    }

    fseek(f_bak, 0, SEEK_END);
    long size = ftell(f_bak);
    fseek(f_bak, 0, SEEK_SET);

    if (size <= 0) {
        log_write("restoreHekateIni: backup is empty or invalid\n");
        fclose(f_bak);
        remove(HEKATE_INI_BAK_PATH);
        return false;
    }

    std::vector<u8> backup_data(size);
    fread(backup_data.data(), 1, size, f_bak);
    fclose(f_bak);

    FILE* f_out = fopen(HEKATE_INI_PATH, "wb");
    if (!f_out) {
        log_write("restoreHekateIni: failed to open %s for writing\n", HEKATE_INI_PATH);
        return false;
    }

    fwrite(backup_data.data(), 1, size, f_out);
    fclose(f_out);

    remove(HEKATE_INI_BAK_PATH);

    fsdevCommitDevice("sdmc");

    log_write("restoreHekateIni: hekate_ipl.ini restored from backup (%ld bytes)\n", size);
    return true;
}

// Check if hekate_ipl.ini backup exists
bool isHekateAutobootActive() {
    FILE* f = fopen(HEKATE_INI_BAK_PATH, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

// Swap payload.bin with HATS installer (no reboot)
// Returns true on success, false on failure
// NOTE: HATS installer payload handles the actual swapping on boot
// This function is kept for potential future use
bool swapPayload(const char* path) {
    constexpr const char* PAYLOAD_BIN = "/payload.bin";
    constexpr const char* PAYLOAD_BAK = "/payload.bak";
    constexpr const char* UPDATE_BIN = "/bootloader/update.bin";
    constexpr const char* UPDATE_BAK = "/bootloader/update.bak";

    log_write("swapPayload: swapping with HATS installer: %s\n", path);

    // Step 1: Read HATS installer into memory
    FILE* f_installer = fopen(path, "rb");
    if (!f_installer) {
        log_write("swapPayload: HATS installer not found: %s\n", path);
        return false;
    }
    fseek(f_installer, 0, SEEK_END);
    long installer_size = ftell(f_installer);
    fseek(f_installer, 0, SEEK_SET);

    if (installer_size <= 0) {
        log_write("swapPayload: invalid HATS installer size: %ld\n", installer_size);
        fclose(f_installer);
        return false;
    }

    std::vector<u8> installer_data(installer_size);
    size_t bytes_read = fread(installer_data.data(), 1, installer_size, f_installer);
    fclose(f_installer);

    if (bytes_read != (size_t)installer_size) {
        log_write("swapPayload: failed to read HATS installer\n");
        return false;
    }
    log_write("swapPayload: read HATS installer (%ld bytes)\n", installer_size);

    fs::FsNativeSd fs;
    fs.CreateDirectory("/bootloader");

    // Helper lambda to swap a payload file
    auto swap_file = [&](const char* src_path, const char* bak_path) {
        FILE* f_src = fopen(src_path, "rb");
        if (!f_src) {
            log_write("swapPayload: %s not found, skipping\n", src_path);
            return;
        }
        fclose(f_src);

        log_write("swapPayload: backing up %s to %s\n", src_path, bak_path);

        // Read original
        f_src = fopen(src_path, "rb");
        fseek(f_src, 0, SEEK_END);
        long size = ftell(f_src);
        fseek(f_src, 0, SEEK_SET);

        if (size > 0) {
            std::vector<u8> original(size);
            fread(original.data(), 1, size, f_src);
            fclose(f_src);

            // Write backup
            FILE* f_bak = fopen(bak_path, "wb");
            if (f_bak) {
                fwrite(original.data(), 1, size, f_bak);
                fclose(f_bak);
                log_write("swapPayload: backed up %s (%ld bytes)\n", src_path, size);
            }

            // Write HATS installer
            FILE* f_dst = fopen(src_path, "wb");
            if (f_dst) {
                fwrite(installer_data.data(), 1, installer_size, f_dst);
                fclose(f_dst);
                log_write("swapPayload: wrote HATS installer to %s (%ld bytes)\n", src_path, installer_size);
            }
        } else {
            fclose(f_src);
        }
    };

    // Step 2: Swap /payload.bin (modchip looks here first)
    swap_file(PAYLOAD_BIN, PAYLOAD_BAK);

    // Step 3: Swap /bootloader/update.bin (modchip fallback)
    swap_file(UPDATE_BIN, UPDATE_BAK);

    // Step 4: Sync filesystem
    log_write("swapPayload: syncing filesystem...\n");
    fsdevCommitDevice("sdmc");

    log_write("swapPayload: swap complete\n");
    return true;
}

// Revert payload swap (restore hekate from backup)
// Returns true if reverted, false if no backup existed
bool revertPayloadSwap() {
    constexpr const char* PAYLOAD_BIN = "/payload.bin";
    constexpr const char* PAYLOAD_BAK = "/payload.bak";
    constexpr const char* UPDATE_BIN = "/bootloader/update.bin";
    constexpr const char* UPDATE_BAK = "/bootloader/update.bak";

    bool reverted = false;

    // Helper lambda to restore a file from backup
    auto restore_file = [&](const char* dst_path, const char* bak_path) {
        FILE* f_bak = fopen(bak_path, "rb");
        if (!f_bak) {
            return;
        }

        fseek(f_bak, 0, SEEK_END);
        long bak_size = ftell(f_bak);
        fseek(f_bak, 0, SEEK_SET);

        if (bak_size > 0) {
            std::vector<u8> backup_data(bak_size);
            fread(backup_data.data(), 1, bak_size, f_bak);
            fclose(f_bak);

            FILE* f_dst = fopen(dst_path, "wb");
            if (f_dst) {
                fwrite(backup_data.data(), 1, bak_size, f_dst);
                fclose(f_dst);
                log_write("revertPayloadSwap: restored %s (%ld bytes)\n", dst_path, bak_size);
                reverted = true;
            }
        } else {
            fclose(f_bak);
        }
        remove(bak_path);
    };

    // Restore all backups
    restore_file(PAYLOAD_BIN, PAYLOAD_BAK);
    restore_file(UPDATE_BIN, UPDATE_BAK);

    if (!reverted) {
        log_write("revertPayloadSwap: no backup found, nothing to revert\n");
        return false;
    }

    // Sync filesystem
    fsdevCommitDevice("sdmc");

    log_write("revertPayloadSwap: revert complete\n");
    return true;
}

// Check if payload swap is currently active (any .bak file exists)
bool isPayloadSwapped() {
    constexpr const char* PAYLOAD_BAK = "/payload.bak";
    constexpr const char* UPDATE_BAK = "/bootloader/update.bak";

    FILE* f = fopen(PAYLOAD_BAK, "rb");
    if (f) {
        fclose(f);
        return true;
    }

    f = fopen(UPDATE_BAK, "rb");
    if (f) {
        fclose(f);
        return true;
    }

    return false;
}

// Swap payload.bin with HATS installer and reboot
// This works on both Erista and Mariko since the system loads sd:\payload.bin
bool rebootToPayload(const char* path) {
    constexpr const char* PAYLOAD_BIN = "/payload.bin";
    constexpr const char* PAYLOAD_BAK = "/payload.bak";

    log_write("rebootToPayload: launching HATS installer from: %s\n", path);

    // Step 1: Check if payload.bin exists
    FILE* f_existing = fopen(PAYLOAD_BIN, "rb");
    if (!f_existing) {
        log_write("rebootToPayload: ERROR - %s not found! This system may not be configured correctly.\n", PAYLOAD_BIN);
        log_write("rebootToPayload: sd:\\payload.bin should contain hekate for normal boot.\n");
        return false;
    }
    fclose(f_existing);

    // Step 2: Backup original payload.bin to payload.bak
    log_write("rebootToPayload: backing up %s to %s\n", PAYLOAD_BIN, PAYLOAD_BAK);

    // Read payload.bin (original hekate)
    FILE* f_src = fopen(PAYLOAD_BIN, "rb");
    if (!f_src) {
        log_write("rebootToPayload: failed to open %s for reading\n", PAYLOAD_BIN);
        return false;
    }

    fseek(f_src, 0, SEEK_END);
    long original_size = ftell(f_src);
    fseek(f_src, 0, SEEK_SET);

    if (original_size <= 0) {
        log_write("rebootToPayload: invalid payload.bin size: %ld\n", original_size);
        fclose(f_src);
        return false;
    }

    std::vector<u8> original_payload(original_size);
    size_t bytes_read = fread(original_payload.data(), 1, original_size, f_src);
    fclose(f_src);

    if (bytes_read != (size_t)original_size) {
        log_write("rebootToPayload: failed to read %s\n", PAYLOAD_BIN);
        return false;
    }

    // Write backup
    FILE* f_bak = fopen(PAYLOAD_BAK, "wb");
    if (!f_bak) {
        log_write("rebootToPayload: failed to create %s\n", PAYLOAD_BAK);
        return false;
    }

    size_t bytes_written = fwrite(original_payload.data(), 1, original_size, f_bak);
    fclose(f_bak);

    if (bytes_written != (size_t)original_size) {
        log_write("rebootToPayload: failed to write backup\n");
        remove(PAYLOAD_BAK);
        return false;
    }

    log_write("rebootToPayload: backup created (%ld bytes)\n", original_size);

    // Step 3: Copy HATS installer to payload.bin
    log_write("rebootToPayload: copying HATS installer to %s\n", PAYLOAD_BIN);

    FILE* f_installer = fopen(path, "rb");
    if (!f_installer) {
        log_write("rebootToPayload: failed to open HATS installer: %s\n", path);
        // Restore backup before returning
        FILE* f_restore = fopen(PAYLOAD_BIN, "wb");
        fwrite(original_payload.data(), 1, original_size, f_restore);
        fclose(f_restore);
        remove(PAYLOAD_BAK);
        return false;
    }

    fseek(f_installer, 0, SEEK_END);
    long installer_size = ftell(f_installer);
    fseek(f_installer, 0, SEEK_SET);

    if (installer_size <= 0) {
        log_write("rebootToPayload: invalid HATS installer size: %ld\n", installer_size);
        fclose(f_installer);
        // Restore backup
        FILE* f_restore = fopen(PAYLOAD_BIN, "wb");
        fwrite(original_payload.data(), 1, original_size, f_restore);
        fclose(f_restore);
        remove(PAYLOAD_BAK);
        return false;
    }

    std::vector<u8> installer_data(installer_size);
    bytes_read = fread(installer_data.data(), 1, installer_size, f_installer);
    fclose(f_installer);

    if (bytes_read != (size_t)installer_size) {
        log_write("rebootToPayload: failed to read HATS installer\n");
        // Restore backup
        FILE* f_restore = fopen(PAYLOAD_BIN, "wb");
        fwrite(original_payload.data(), 1, original_size, f_restore);
        fclose(f_restore);
        remove(PAYLOAD_BAK);
        return false;
    }

    // Write HATS installer to payload.bin
    FILE* f_dst = fopen(PAYLOAD_BIN, "wb");
    if (!f_dst) {
        log_write("rebootToPayload: failed to open %s for writing\n", PAYLOAD_BIN);
        // Restore backup
        FILE* f_restore = fopen(PAYLOAD_BIN, "wb");
        fwrite(original_payload.data(), 1, original_size, f_restore);
        fclose(f_restore);
        remove(PAYLOAD_BAK);
        return false;
    }

    bytes_written = fwrite(installer_data.data(), 1, installer_size, f_dst);
    fclose(f_dst);

    if (bytes_written != (size_t)installer_size) {
        log_write("rebootToPayload: failed to write HATS installer\n");
        // Restore backup
        FILE* f_restore = fopen(PAYLOAD_BIN, "wb");
        fwrite(original_payload.data(), 1, original_size, f_restore);
        fclose(f_restore);
        remove(PAYLOAD_BAK);
        return false;
    }

    // Step 4: Sync filesystem
    fsdevCommitDevice("sdmc");

    // Small delay to ensure SD card finishes writing (important for Erista)
    svcSleepThread(500'000'000ULL); // 500ms

    log_write("rebootToPayload: payload swapped (%ld bytes), rebooting...\n", installer_size);

    // Step 5: Reboot - system will load sd:\payload.bin (which is now HATS installer)
    log_write("rebootToPayload: HATS installer will restore hekate after installation\n");

    spsmInitialize();
    spsmShutdown(true);

    // Should not reach here
    return false;
}

} // namespace sphaira::utils
