#pragma once

#include <string>
#include <switch.h>

namespace sphaira::hats {

// Scan SD root for HATS-*.txt file and return version string
// Returns "Not Found" if no version file exists
std::string getHatsVersion();

// Get system firmware version via setsysGetFirmwareVersion()
// Returns version string like "19.0.1"
std::string getSystemFirmware();

// Get Atmosphere version via splGetConfig(65000)
// Returns version string like "1.8.0" with |E or |S suffix
std::string getAtmosphereVersion();

// Get combined info string for display
// Returns format: "HATS-xxx; Atmosphere: x.x.x|E/S"
std::string getAmsInfo();

// Check if running Atmosphere CFW
bool isAtmosphere();

// Check if running on Erista (v1) or Mariko (v2) hardware
bool isErista();

} // namespace sphaira::hats
