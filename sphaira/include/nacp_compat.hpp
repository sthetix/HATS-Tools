#pragma once

#include <switch.h>

#include <cstddef>
#include <span>

namespace sphaira {

constexpr std::size_t NacpLanguageEntryCount = 16;

// libnx fdf3c870 renamed NacpStruct::lang to NacpStruct::lang_data.lang.
// The language-data block remains the first 0x3000 bytes of the binary NACP
// layout in both versions, so use that stable layout instead of either member
// name. Compressed HOS 21 language data must still be normalized before these
// entries are read.
static_assert(sizeof(NacpLanguageEntry) * NacpLanguageEntryCount == 0x3000);
static_assert(offsetof(NacpStruct, isbn) == 0x3000);

inline auto NacpLanguageEntries(NacpStruct& nacp) noexcept
    -> std::span<NacpLanguageEntry, NacpLanguageEntryCount> {
    return std::span<NacpLanguageEntry, NacpLanguageEntryCount>{
        reinterpret_cast<NacpLanguageEntry*>(&nacp), NacpLanguageEntryCount};
}

inline auto NacpLanguageEntries(const NacpStruct& nacp) noexcept
    -> std::span<const NacpLanguageEntry, NacpLanguageEntryCount> {
    return std::span<const NacpLanguageEntry, NacpLanguageEntryCount>{
        reinterpret_cast<const NacpLanguageEntry*>(&nacp), NacpLanguageEntryCount};
}

} // namespace sphaira
