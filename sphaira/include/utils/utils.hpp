#pragma once

#include "ui/types.hpp"

namespace sphaira::utils {

struct HashStr {
    char str[0x21];
};

HashStr hexIdToStr(FsRightsId id);
HashStr hexIdToStr(NcmRightsId id);
HashStr hexIdToStr(NcmContentId id);

template<typename T>
constexpr inline T AlignUp(T value, T align) {
    return (value + (align - 1)) &~ (align - 1);
}

template<typename T>
constexpr inline T AlignDown(T value, T align) {
    return value &~ (align - 1);
}

// formats size to 1.23 MB in 1024 base.
// only uses 32 bytes so its SSO optimised, not need to cache.
std::string formatSizeStorage(u64 size);

// formats size to 1.23 MB in 1000 base (used for progress bars).
std::string formatSizeNetwork(u64 size);

// Set hekate_ipl.ini to auto-boot HATS installer payload
// Backs up original ini and modifies config to autoboot the payload
// Returns true on success, false on failure
bool setHekateAutobootPayload(const char* payload_path);

// Restore hekate_ipl.ini from backup
// Returns true if restored, false if no backup existed
bool restoreHekateIni();

// Check if hekate_ipl.ini backup exists (autoboot is active)
bool isHekateAutobootActive();

// Swap payload.bin with HATS installer (no reboot)
// Returns true on success, false on failure
bool swapPayload(const char* path);

// Revert payload swap (restore hekate from backup)
// Returns true if reverted, false if no backup existed
bool revertPayloadSwap();

// Check if payload swap is currently active (payload.bak exists)
bool isPayloadSwapped();

// Reboot to a payload file (HATS installer)
// Swaps sd:\payload.bin with the installer, then reboots
// Returns true on success, false on failure
bool rebootToPayload(const char* path);

} // namespace sphaira::utils
