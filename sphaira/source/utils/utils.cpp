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

// Secure Monitor call for IRAM copy (smcAmsIramCopy)
static void smcCopyToIram(uintptr_t iram_addr, const void* src_addr, u32 size) {
    SecmonArgs args = {};
    args.X[0] = 0xF0000201;     /* smcAmsIramCopy */
    args.X[1] = (u64)src_addr;  /* DRAM address */
    args.X[2] = (u64)iram_addr; /* IRAM address */
    args.X[3] = size;           /* Amount to copy */
    args.X[4] = 1;              /* 1 = Write */
    svcCallSecureMonitor(&args);
}

// Set reboot to payload mode
static void smcRebootToIramPayload(void) {
    splInitialize();
    splSetConfig((SplConfigItem)65001, 2);  // NeedsReboot = 2 (reboot to IRAM payload)
    splExit();
}

bool rebootToPayload(const char* path) {
    log_write("rebootToPayload: attempting to load payload from: %s\n", path);

    // Open the payload file using stdio (simpler, uses fsdev under the hood)
    FILE* f = fopen(path, "rb");
    if (!f) {
        log_write("rebootToPayload: failed to open file\n");
        return false;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    log_write("rebootToPayload: payload file size: %ld bytes (max: %u)\n", size, IRAM_PAYLOAD_MAX_SIZE);

    // Check payload size
    if (size <= 0) {
        log_write("rebootToPayload: invalid payload size (<= 0)\n");
        fclose(f);
        return false;
    }
    if (size > IRAM_PAYLOAD_MAX_SIZE) {
        log_write("rebootToPayload: payload too large (> %u)\n", IRAM_PAYLOAD_MAX_SIZE);
        fclose(f);
        return false;
    }

    // Read payload in chunks and copy to IRAM
    alignas(0x1000) u8 page_buf[0x1000];
    bool success = true;
    size_t offset = 0;

    while (offset < (size_t)size) {
        size_t bytes_to_read = sizeof(page_buf);
        if (offset + bytes_to_read > (size_t)size) {
            bytes_to_read = (size_t)size - offset;
        }

        size_t bytes_read = fread(page_buf, 1, bytes_to_read, f);
        if (bytes_read == 0) {
            log_write("rebootToPayload: failed to read payload at offset %zu\n", offset);
            success = false;
            break;
        }

        smcCopyToIram(AMS_IWRAM_OFFSET + offset, page_buf, bytes_read);
        offset += bytes_read;
    }

    fclose(f);

    if (success) {
        log_write("rebootToPayload: payload loaded to IRAM, triggering reboot...\n");
        smcRebootToIramPayload();
    } else {
        log_write("rebootToPayload: failed to load payload to IRAM\n");
    }

    return success;
}

} // namespace sphaira::utils
