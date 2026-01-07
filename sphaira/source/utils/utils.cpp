#include "utils/utils.hpp"
#include "log.hpp"

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

    log_write("rebootToPayload: payload swapped (%ld bytes), rebooting...\n", installer_size);

    // Step 5: Reboot - system will load sd:\payload.bin (which is now HATS installer)
    log_write("rebootToPayload: HATS installer will restore hekate after installation\n");

    spsmInitialize();
    spsmShutdown(true);

    // Should not reach here
    return false;
}

} // namespace sphaira::utils
