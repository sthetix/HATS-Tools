#pragma once

#include "fs.hpp"
#include <string>
#include <vector>
#include <map>

namespace sphaira::manifest {

// Path to manifest on SD card root
constexpr const char* MANIFEST_PATH = "/manifest.json";

// Protected component IDs that cannot be uninstalled
constexpr const char* PROTECTED_COMPONENTS[] = {
    "atmosphere",
    "hekate"
};

struct Component {
    std::string id;
    std::string name;
    std::string version;
    std::string category;
    std::string repo;
    std::vector<std::string> files;

    bool isProtected() const;
};

struct Manifest {
    std::string pack_name;
    std::string build_date;
    std::string builder_version;
    std::string supported_firmware;
    std::string content_hash;
    std::map<std::string, Component> components;
};

// Load manifest from /manifest.json
// Returns true on success, false if file doesn't exist or parse error
bool load(Manifest& out);

// Save manifest to /manifest.json
// Returns true on success
bool save(const Manifest& m);

// Check if manifest exists
bool exists();

// Get list of all components (for uninstaller menu)
std::vector<Component> getComponents(const Manifest& m);

// Get list of uninstallable components (excludes atmosphere/hekate)
std::vector<Component> getUninstallableComponents(const Manifest& m);

// Remove a component from manifest and delete its files
// Returns true on success
bool removeComponent(Manifest& m, const std::string& id, fs::Fs* fs);

// Remove multiple components
// Returns number of successfully removed components
int removeComponents(Manifest& m, const std::vector<std::string>& ids, fs::Fs* fs);

// Check if a component ID is protected
bool isProtectedComponent(const std::string& id);

} // namespace sphaira::manifest
