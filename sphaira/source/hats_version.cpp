#include "hats_version.hpp"
#include "fs.hpp"
#include "log.hpp"

#include <dirent.h>
#include <cstring>
#include <string>

namespace sphaira::hats {

namespace {

bool isPost019() {
    u64 version;
    if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &version))) {
        if (((version >> 56) & ((1 << 8) - 1)) > 0 || ((version >> 48) & ((1 << 8) - 1)) >= 19) {
            return true;
        }
    }
    return false;
}

Result smAtmosphereHasService(bool* out, SmServiceName name, bool v019) {
    u8 tmp = 0;
    Result rc = v019 ? tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, tmp)
                     : serviceDispatchInOut(smGetServiceSession(), 65100, name, tmp);
    if (R_SUCCEEDED(rc) && out)
        *out = tmp;
    return rc;
}

} // namespace

std::string getHatsVersion() {
    std::string hatsVersion = "Not Found";

    auto dir = opendir("/");
    if (!dir) {
        return hatsVersion;
    }

    while (auto d = readdir(dir)) {
        if (d->d_type != DT_REG) {
            continue;
        }

        const char* name = d->d_name;
        size_t len = std::strlen(name);

        // Check if filename starts with "HATS-" and ends with ".txt"
        if (len > 9 && std::strncmp(name, "HATS-", 5) == 0 &&
            std::strcmp(name + len - 4, ".txt") == 0) {
            // Extract version without .txt extension
            hatsVersion = std::string(name, len - 4);
            break;
        }
    }

    closedir(dir);
    return hatsVersion;
}

std::string getSystemFirmware() {
    SetSysFirmwareVersion ver;
    if (R_SUCCEEDED(setsysInitialize())) {
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&ver))) {
            setsysExit();
            return ver.display_version;
        }
        setsysExit();
    }
    return "Unknown";
}

std::string getAtmosphereVersion() {
    u64 version;
    std::string res = "Unknown";

    // Initialize spl service
    if (R_FAILED(splInitialize())) {
        return res;
    }

    if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &version))) {
        res = std::to_string((version >> 56) & ((1 << 8) - 1)) + "." +
              std::to_string((version >> 48) & ((1 << 8) - 1)) + "." +
              std::to_string((version >> 40) & ((1 << 8) - 1));

        u64 emummc;
        if (R_SUCCEEDED(splGetConfig((SplConfigItem)65007, &emummc))) {
            res += emummc ? "|E" : "|S";
        }
    }

    splExit();
    return res;
}

std::string getAmsInfo() {
    std::string hatsVer = getHatsVersion();
    std::string amsVer = getAtmosphereVersion();

    return hatsVer + "; Atmosphere: " + amsVer;
}

bool isAtmosphere() {
    bool res = false;
    bool v019 = isPost019();

    // Try AMS-specific service check
    if (R_SUCCEEDED(smAtmosphereHasService(&res, smEncodeName("ams"), v019))) {
        return res;
    }

    // Fallback: check if we can query AMS version
    u64 version;
    return R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &version));
}

bool isErista() {
    u64 hardware_type;
    bool result = true; // Default to Erista if unknown

    if (R_FAILED(splInitialize())) {
        return result;
    }

    if (R_SUCCEEDED(splGetConfig(SplConfigItem_HardwareType, &hardware_type))) {
        // Erista types are 0 (Icosa), 1 (Copper)
        // Mariko types are 2 (Hoag), 3 (Iowa), 4 (Calcio), 5 (Aula)
        result = hardware_type <= 1;
    }

    splExit();
    return result;
}

} // namespace sphaira::hats
