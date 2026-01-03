#include "ui/menus/cheats_menu.hpp"

#include "ui/nvg_util.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/error_box.hpp"
#include "ui/scrollable_text.hpp"

#include "app.hpp"
#include "log.hpp"
#include "download.hpp"
#include "fs.hpp"
#include "i18n.hpp"
#include "yyjson_helper.hpp"
#include "swkbd.hpp"
#include "yati/nx/ns.hpp"
#include "yati/nx/es.hpp"

#include <yyjson.h>
#include <cstring>
#include <format>
#include <ranges>
#include <sstream>
#include <switch.h>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <map>

namespace sphaira::ui::menu::hats {

namespace {

// Constants for CheatSlips API
constexpr const char* CHEATSLIPS_API_URL = "https://www.cheatslips.com/api/v1/cheats";
constexpr const char* CHEATSLIPS_TOKEN_URL = "https://www.cheatslips.com/api/v1/token";
// Online versions database for build ID lookup (from switch-cheats-db)
constexpr const char* VERSIONS_DB_URL = "https://raw.githubusercontent.com/HamletDuFromage/switch-cheats-db/master/versions";

// Paths for storing cheats and tokens
constexpr const char* ATMOSPHERE_CONTENTS_PATH = "/atmosphere/contents";
constexpr const char* CHEATS_SUBDIR = "cheats";
constexpr const char* TOKEN_PATH = "/config/hats-tools/cheatslips_token.json";

// nx-cheats-db local database paths (optional cache)
constexpr const char* NX_DB_PATH = "/config/hats-tools/cheats-db";
constexpr const char* NX_DB_VERSIONS_FILE = "versions.json";
// nx-cheats-db GitHub raw URLs
constexpr const char* NX_DB_GITHUB_BASE = "https://raw.githubusercontent.com/sthetix/nx-cheats-db/main";
constexpr const char* NX_DB_VERSIONS_URL = "https://raw.githubusercontent.com/sthetix/nx-cheats-db/main/versions.json";
// AIO-Switch-Updater token path (for compatibility)
constexpr const char* AIO_TOKEN_PATH = "/config/aio-switch-updater/token.json";

// Number of titles to fetch per chunk (like original sphaira)
constexpr s32 ENTRY_CHUNK_COUNT = 1000;

// DmntCheatProcessMetadata structure for getting build ID from dmnt:cht
struct DmntCheatProcessMetadata {
    u64 process_id;
    u64 title_id;
    u8 main_nso_build_id[0x20];
    u8 padding[0xB0];
};

// Format title ID as 16-character hex string (lowercase for atmosphere paths)
auto FormatTitleId(u64 title_id) -> std::string {
    return std::format("{:016X}", title_id);
}

// Format title ID as lowercase for file paths
auto FormatTitleIdLower(u64 title_id) -> std::string {
    return std::format("{:016x}", title_id);
}

// Convert bytes to hex string (uppercase)
auto BytesToHex(const u8* data, size_t len) -> std::string {
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02X", data[i]);
        hex += buf;
    }
    return hex;
}

// Case-insensitive string comparison
auto StringsEqualIgnoreCase(const std::string& a, const std::string& b) -> bool {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), [](char c1, char c2) {
        return std::tolower(c1) == std::tolower(c2);
    });
}

// Get Build ID from dmnt:cht service (when game is running/suspended)
auto GetBuildIdFromDmnt(u64 title_id) -> std::string {
    Service dmntchtSrv;
    Result rc = smGetService(&dmntchtSrv, "dmnt:cht");
    if (R_FAILED(rc)) {
        log_write("[Cheats] Failed to get dmnt:cht service: %x\n", rc);
        return "";
    }

    ON_SCOPE_EXIT(serviceClose(&dmntchtSrv));

    // Initialize dmnt:cht (command 65003)
    rc = serviceDispatch(&dmntchtSrv, 65003);
    if (R_FAILED(rc)) {
        log_write("[Cheats] Failed to initialize dmnt:cht: %x\n", rc);
        return "";
    }

    // Get process metadata (command 65002)
    DmntCheatProcessMetadata metadata{};
    rc = serviceDispatchOut(&dmntchtSrv, 65002, metadata);
    if (R_FAILED(rc)) {
        log_write("[Cheats] Failed to get cheat metadata: %x\n", rc);
        return "";
    }

    log_write("[Cheats] dmnt:cht metadata - title_id: %016lx, process_id: %016lx\n",
              metadata.title_id, metadata.process_id);

    // Check if the running game matches our target title_id
    if (metadata.title_id != title_id) {
        log_write("[Cheats] Running title %016lx doesn't match target %016lx\n",
                  metadata.title_id, title_id);
        return "";
    }

    // Build ID is in main_nso_build_id, first 8 bytes are the build ID
    std::string build_id = BytesToHex(metadata.main_nso_build_id, 8);
    log_write("[Cheats] Got Build ID from dmnt:cht: %s\n", build_id.c_str());

    return build_id;
}

// Get version for a title (like aio-switch-updater does)
auto GetTitleVersion(u64 title_id) -> u32 {
    u32 version = 0;
    s32 out = 0;

    // Use title namespace functions to get meta entries
    s32 count = 0;
    Result rc = nsCountApplicationContentMeta(title_id, &count);
    if (R_FAILED(rc) || count == 0) {
        return 0;
    }

    std::vector<NsApplicationContentMetaStatus> meta_statuses(count);
    rc = nsListApplicationContentMetaStatus(title_id, 0, meta_statuses.data(), meta_statuses.size(), &out);
    if (R_FAILED(rc)) {
        return 0;
    }

    meta_statuses.resize(out);

    // Find the highest version
    for (const auto& meta : meta_statuses) {
        if (meta.version > version) {
            version = meta.version;
        }
    }

    return version;
}

// Get title name using nsGetApplicationControlData
auto GetTitleName(u64 title_id) -> std::string {
    NsApplicationControlData control_data;
    u64 actual_size = 0;

    Result rc = nsGetApplicationControlData(
        NsApplicationControlSource_Storage,
        title_id,
        &control_data,
        sizeof(control_data),
        &actual_size
    );

    if (R_FAILED(rc)) {
        // Try cache only as fallback
        rc = nsGetApplicationControlData(
            NsApplicationControlSource_CacheOnly,
            title_id,
            &control_data,
            sizeof(control_data),
            &actual_size
        );
        if (R_FAILED(rc)) {
            return "";
        }
    }

    // Get language entry for the name
    NacpLanguageEntry* lang_entry = nullptr;
    rc = nacpGetLanguageEntry(&control_data.nacp, &lang_entry);
    if (R_FAILED(rc) || !lang_entry || !lang_entry->name) {
        return "";
    }

    return lang_entry->name;
}

// Clean cheat content by removing non-cheat entries (credits, website headers)
auto CleanCheatContent(const std::string& content) -> std::string {
    std::istringstream stream(content);
    std::string line;
    std::string cleaned_content;
    bool in_cheat = false;
    std::string current_cheat_name;

    while (std::getline(stream, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        // Skip empty lines
        if (line.empty()) {
            if (in_cheat) {
                cleaned_content += "\n";
            }
            continue;
        }

        // Check if this is a cheat title line [Title]
        if (line.size() > 2 && line[0] == '[' && line.back() == ']') {
            std::string title = line.substr(1, line.length() - 2);
            std::string lower_title = title;
            std::transform(lower_title.begin(), lower_title.end(), lower_title.begin(), ::tolower);

            // Check if this should be skipped
            bool should_skip = false;
            if (lower_title.find("www.") != std::string::npos) should_skip = true;  // Website URLs
            if (lower_title.find("credits:") == 0) should_skip = true;  // credits: author
            if (lower_title.find("credit:") == 0) should_skip = true;   // credit: author
            if (lower_title == "credits") should_skip = true;
            if (lower_title == "credit") should_skip = true;

            if (should_skip) {
                // Skip this entry and its content until next cheat
                in_cheat = false;
                continue;
            }

            // Valid cheat title, add it
            cleaned_content += line + "\n";
            in_cheat = true;
        } else if (in_cheat) {
            // Add content lines if we're in a valid cheat
            cleaned_content += line + "\n";
        }
    }

    // Remove trailing newlines
    while (!cleaned_content.empty() && (cleaned_content.back() == '\n' || cleaned_content.back() == '\r')) {
        cleaned_content.pop_back();
    }

    return cleaned_content;
}

// Parse CheatSlips JSON response and filter by Build ID
// API returns: [{"slug": "xxx", "name": "xxx", "cheats": [{"id": 0, "buildid": "xxx", "content": "xxx", "credits": "xxx", "titles": ["cheat1", "cheat2"]}]}]
auto ParseCheatslipsCheats(const std::string& json_str, const std::string& target_build_id) -> std::vector<CheatEntry> {
    std::vector<CheatEntry> cheats;

    // Log raw response for debugging
    log_write("[Cheats] Parsing API response, target Build ID: %s\n", target_build_id.c_str());

    yyjson_doc* doc = yyjson_read(json_str.data(), json_str.size(), 0);
    if (!doc) {
        log_write("[Cheats] Failed to parse CheatSlips JSON\n");
        return cheats;
    }

    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    yyjson_val* root = yyjson_doc_get_root(doc);

    // Response is an array with a single game object
    if (yyjson_is_arr(root)) {
        log_write("[Cheats] Response is an array\n");
        // Get the first (and usually only) game object
        size_t idx, max;
        yyjson_val* game_val;
        yyjson_arr_foreach(root, idx, max, game_val) {
            if (!yyjson_is_obj(game_val)) continue;

            // Get game name for context
            yyjson_val* name_val = yyjson_obj_get(game_val, "name");
            std::string game_name = name_val && yyjson_is_str(name_val) ? yyjson_get_str(name_val) : "";

            // Get cheats array for this version
            yyjson_val* cheats_arr = yyjson_obj_get(game_val, "cheats");
            if (!cheats_arr || !yyjson_is_arr(cheats_arr)) continue;

            log_write("[Cheats] Processing game: %s with %zu cheat entries\n", game_name.c_str(), yyjson_arr_size(cheats_arr));

            size_t cheat_idx, cheat_max;
            yyjson_val* cheat_val;
            yyjson_arr_foreach(cheats_arr, cheat_idx, cheat_max, cheat_val) {
                if (!yyjson_is_obj(cheat_val)) continue;

                // Get buildid (note: lowercase field name in API)
                yyjson_val* build_id_val = yyjson_obj_get(cheat_val, "buildid");
                std::string build_id = build_id_val && yyjson_is_str(build_id_val) ? yyjson_get_str(build_id_val) : "";

                // Get content
                yyjson_val* content_val = yyjson_obj_get(cheat_val, "content");
                if (!content_val || !yyjson_is_str(content_val)) continue;

                const char* content = yyjson_get_str(content_val);

                // Check if API returned quota exceeded message
                if (strstr(content, "Quota exceeded") || strstr(content, "quota exceeded")) {
                    log_write("[Cheats] API quota exceeded, skipping cheat\n");
                    continue; // Skip quota-exceeded cheats entirely
                }

                // Get credits (optional)
                yyjson_val* credits_val = yyjson_obj_get(cheat_val, "credits");
                std::string credits = credits_val && yyjson_is_str(credits_val) ? yyjson_get_str(credits_val) : "";

                // Only add cheats matching the target Build ID (case-insensitive)
                if (!target_build_id.empty() && !StringsEqualIgnoreCase(build_id, target_build_id)) {
                    log_write("[Cheats] Skipping cheat with Build ID: %s (target: %s)\n",
                              build_id.c_str(), target_build_id.c_str());
                    continue;
                }

                // Get titles array and parse into individual cheat entries
                // CheatSlips returns ALL cheats in a single content field
                // We need to parse it and create individual entries for each cheat
                std::string raw_content = yyjson_get_str(content_val);
                std::string cleaned_content = CleanCheatContent(raw_content);

                // Parse the cleaned content to extract individual cheats
                std::istringstream content_stream(cleaned_content);
                std::string line;
                std::string current_cheat_name;
                std::string current_cheat_content;
                bool in_cheat = false;

                while (std::getline(content_stream, line)) {
                    // Trim whitespace
                    line.erase(0, line.find_first_not_of(" \t\r\n"));
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);

                    // Check for cheat title [Title]
                    if (!line.empty() && line.size() > 2 && line[0] == '[' && line.back() == ']') {
                        // Save previous cheat if exists
                        if (in_cheat && !current_cheat_name.empty()) {
                            CheatEntry entry;
                            entry.name = current_cheat_name;
                            entry.content = current_cheat_content;
                            entry.build_id = build_id;
                            entry.source = CheatSource::Cheatslips;
                            entry.selected = false;
                            cheats.push_back(std::move(entry));
                            log_write("[Cheats] Parsed cheat: %s\n", current_cheat_name.c_str());
                        }

                        // Start new cheat
                        current_cheat_name = line.substr(1, line.length() - 2);
                        current_cheat_content = line + "\n";
                        in_cheat = true;
                    } else if (in_cheat) {
                        // Add line to current cheat content
                        current_cheat_content += line + "\n";
                    }
                }

                // Don't forget the last cheat
                if (in_cheat && !current_cheat_name.empty()) {
                    CheatEntry entry;
                    entry.name = current_cheat_name;
                    entry.content = current_cheat_content;
                    entry.build_id = build_id;
                    entry.source = CheatSource::Cheatslips;
                    entry.selected = false;
                    cheats.push_back(std::move(entry));
                    log_write("[Cheats] Parsed cheat: %s\n", current_cheat_name.c_str());
                }

                log_write("[Cheats] Total cheats parsed from CheatSlips: %zu\n", cheats.size());
            }
        }
    } else if (yyjson_is_obj(root)) {
        log_write("[Cheats] Response is an object (error or single game)\n");
        // Check for error message
        yyjson_val* error_val = yyjson_obj_get(root, "error");
        if (error_val && yyjson_is_str(error_val)) {
            log_write("[Cheats] API Error: %s\n", yyjson_get_str(error_val));
        }
        // Check for message (like "Quota exceeded")
        yyjson_val* msg_val = yyjson_obj_get(root, "message");
        if (msg_val && yyjson_is_str(msg_val)) {
            log_write("[Cheats] API Message: %s\n", yyjson_get_str(msg_val));
        }

        // Check if response has "cheats" field directly
        yyjson_val* cheats_arr = yyjson_obj_get(root, "cheats");
        if (cheats_arr && yyjson_is_arr(cheats_arr)) {
            log_write("[Cheats] Found cheats array in object response\n");
            size_t cheat_idx, cheat_max;
            yyjson_val* cheat_val;
            yyjson_arr_foreach(cheats_arr, cheat_idx, cheat_max, cheat_val) {
                if (!yyjson_is_obj(cheat_val)) continue;

                yyjson_val* build_id_val = yyjson_obj_get(cheat_val, "buildid");
                std::string build_id = build_id_val && yyjson_is_str(build_id_val) ? yyjson_get_str(build_id_val) : "";

                yyjson_val* content_val = yyjson_obj_get(cheat_val, "content");
                if (!content_val || !yyjson_is_str(content_val)) continue;

                yyjson_val* credits_val = yyjson_obj_get(cheat_val, "credits");
                std::string credits = credits_val && yyjson_is_str(credits_val) ? yyjson_get_str(credits_val) : "";

                // Only add cheats matching the target Build ID (case-insensitive)
                if (!target_build_id.empty() && !StringsEqualIgnoreCase(build_id, target_build_id)) {
                    continue;
                }

                // Parse content into individual cheat entries
                std::string raw_content = yyjson_get_str(content_val);
                std::string cleaned_content = CleanCheatContent(raw_content);

                // Parse the cleaned content to extract individual cheats
                std::istringstream content_stream(cleaned_content);
                std::string line;
                std::string current_cheat_name;
                std::string current_cheat_content;
                bool in_cheat = false;

                while (std::getline(content_stream, line)) {
                    // Trim whitespace
                    line.erase(0, line.find_first_not_of(" \t\r\n"));
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);

                    // Check for cheat title [Title]
                    if (!line.empty() && line.size() > 2 && line[0] == '[' && line.back() == ']') {
                        // Save previous cheat if exists
                        if (in_cheat && !current_cheat_name.empty()) {
                            CheatEntry entry;
                            entry.name = current_cheat_name;
                            entry.content = current_cheat_content;
                            entry.build_id = build_id;
                            entry.source = CheatSource::Cheatslips;
                            entry.selected = false;
                            cheats.push_back(std::move(entry));
                        }

                        // Start new cheat
                        current_cheat_name = line.substr(1, line.length() - 2);
                        current_cheat_content = line + "\n";
                        in_cheat = true;
                    } else if (in_cheat) {
                        // Add line to current cheat content
                        current_cheat_content += line + "\n";
                    }
                }

                // Don't forget the last cheat
                if (in_cheat && !current_cheat_name.empty()) {
                    CheatEntry entry;
                    entry.name = current_cheat_name;
                    entry.content = current_cheat_content;
                    entry.build_id = build_id;
                    entry.source = CheatSource::Cheatslips;
                    entry.selected = false;
                    cheats.push_back(std::move(entry));
                }
            }
        }
    }

    log_write("[Cheats] Parsed %zu cheats from CheatSlips matching Build ID %s\n",
              cheats.size(), target_build_id.c_str());
    return cheats;
}

// Parse nx-cheats-db JSON file
// Format: {"BUILD_ID": {"[Cheat Name]": "[Cheat Name]\n[cheat code]\n\n", ...}, "attribution": {...}}
auto ParseNxDbCheats(const std::string& json_str, const std::string& target_build_id) -> std::vector<CheatEntry> {
    std::vector<CheatEntry> cheats;

    log_write("[Cheats] Parsing nx-cheats-db JSON, target Build ID: %s\n", target_build_id.c_str());

    yyjson_doc* doc = yyjson_read(json_str.data(), json_str.size(), 0);
    if (!doc) {
        log_write("[Cheats] Failed to parse nx-cheats-db JSON\n");
        return cheats;
    }

    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        log_write("[Cheats] nx-cheats-db JSON root is not an object\n");
        return cheats;
    }

    // Look for the target build ID in the JSON
    yyjson_val* build_id_val = yyjson_obj_get(root, target_build_id.c_str());

    // If not found, try to find any build ID (for fallback)
    if (!build_id_val || !yyjson_is_obj(build_id_val)) {
        log_write("[Cheats] Build ID %s not found in nx-cheats-db, checking available keys\n", target_build_id.c_str());

        // List available build IDs for debugging
        yyjson_val* key;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(root, &iter);
        std::string first_build_id;
        while ((key = yyjson_obj_iter_next(&iter))) {
            const char* key_str = yyjson_get_str(key);
            if (key_str && std::string(key_str) != "attribution") {
                if (first_build_id.empty()) {
                    first_build_id = key_str;
                }
                log_write("[Cheats] Available Build ID: %s\n", key_str);
            }
        }

        // Fall back to first available build ID if target not found
        if (!first_build_id.empty()) {
            log_write("[Cheats] Using fallback Build ID: %s\n", first_build_id.c_str());
            build_id_val = yyjson_obj_get(root, first_build_id.c_str());
        }
    }

    if (!build_id_val || !yyjson_is_obj(build_id_val)) {
        log_write("[Cheats] No valid cheat data found in nx-cheats-db JSON\n");
        return cheats;
    }

    // Parse cheats from the build ID object
    yyjson_val* key;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(build_id_val, &iter);

    while ((key = yyjson_obj_iter_next(&iter))) {
        const char* cheat_name = yyjson_get_str(key);
        yyjson_val* cheat_content_val = yyjson_obj_iter_get_val(key);

        if (!cheat_content_val || !yyjson_is_str(cheat_content_val)) {
            continue;
        }

        const char* content = yyjson_get_str(cheat_content_val);

        // Try to extract name from key first
        std::string name_str;
        std::string content_str = content;

        if (cheat_name && strlen(cheat_name) > 0) {
            name_str = cheat_name;

            // Check if this is a game header in format {- Game Name -}
            // Convert it to [Game Name] format
            if (name_str.size() > 2 && name_str[0] == '{' && name_str[name_str.size() - 1] == '}') {
                // Remove braces and dashes
                std::string inner = name_str.substr(1, name_str.size() - 2);
                // Remove leading/trailing dashes and spaces
                size_t start = inner.find_first_not_of("- ");
                size_t end = inner.find_last_not_of("- ");
                if (start != std::string::npos && end != std::string::npos) {
                    inner = inner.substr(start, end - start + 1);
                }
                name_str = inner;
            }
            // Extract cheat name without brackets if present
            else if (name_str.size() > 2 && name_str[0] == '[' && name_str[name_str.size() - 1] == ']') {
                name_str = name_str.substr(1, name_str.size() - 2);
            }
        } else {
            // Key is empty, try to extract name from content (format: "[Name]\n{codes}")
            size_t bracket_start = content_str.find('[');
            size_t bracket_end = content_str.find(']', bracket_start);
            if (bracket_start != std::string::npos && bracket_end != std::string::npos) {
                name_str = content_str.substr(bracket_start + 1, bracket_end - bracket_start - 1);
            } else {
                name_str = "Unknown Cheat";
            }
        }

        // Filter out non-cheat entries
        std::string lower_name = name_str;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

        // Skip metadata files (attribution entries)
        if (lower_name.find(".txt") != std::string::npos) continue;

        // Skip credits, website headers, etc.
        if (lower_name.find("www.") != std::string::npos) continue;
        if (lower_name.find("cheatslips") != std::string::npos) continue;
        if (lower_name.find("credits:") == 0) continue;
        if (lower_name.find("credit:") == 0) continue;
        if (lower_name == "credits") continue;
        if (lower_name == "credit") continue;
        if (lower_name.find("original code by") == 0) continue;  // Skip attribution entries

        // Skip entries with empty content (metadata headers only)
        std::string content_check = content_str;
        size_t first_newline_temp = content_check.find('\n');
        if (first_newline_temp != std::string::npos) {
            content_check = content_check.substr(first_newline_temp + 1);
        }
        // Trim whitespace
        content_check.erase(0, content_check.find_first_not_of(" \t\r\n"));
        content_check.erase(content_check.find_last_not_of(" \t\r\n") + 1);
        if (content_check.empty()) {
            log_write("[Cheats] Skipping entry with empty content: %s\n", name_str.c_str());
            continue;
        }

        // Process content: remove duplicate title line if it exists
        // The content might start with "{- Game Name -}\n" or "[Game Name]\n"
        // We need to remove this first line to avoid duplication
        std::string processed_content = content_str;
        size_t first_newline = processed_content.find('\n');
        if (first_newline != std::string::npos) {
            std::string first_line = processed_content.substr(0, first_newline);
            std::string first_line_lower = first_line;
            std::transform(first_line_lower.begin(), first_line_lower.end(), first_line_lower.begin(), ::tolower);

            // Check if first line matches the name (with or without brackets/dashes)
            bool matches = false;
            if (first_line.size() > 2 && first_line[0] == '[' && first_line[first_line.size() - 1] == ']') {
                std::string first_line_name = first_line.substr(1, first_line.size() - 2);
                if (first_line_name == name_str || first_line_lower.find(lower_name) != std::string::npos) {
                    matches = true;
                }
            }

            // Check for {- Game Name -} format
            if (first_line.size() > 2 && first_line[0] == '{' && first_line[first_line.size() - 1] == '}') {
                if (first_line_lower.find(lower_name) != std::string::npos) {
                    matches = true;
                }
            }

            // Remove first line if it's a duplicate title
            if (matches) {
                processed_content = processed_content.substr(first_newline + 1);
                log_write("[Cheats] Removed duplicate title line from content\n");
            }
        }

        CheatEntry entry;
        entry.name = name_str;
        entry.content = processed_content;
        entry.build_id = target_build_id;
        entry.source = CheatSource::NxDb;
        entry.selected = false;

        cheats.push_back(std::move(entry));
        log_write("[Cheats] Added nx-cheats-db cheat: %s\n", entry.name.c_str());
    }

    log_write("[Cheats] Parsed %zu cheats from nx-cheats-db for Build ID %s\n",
              cheats.size(), target_build_id.c_str());
    return cheats;
}

// Load versions.json from nx-cheats-db
struct NxDbVersionInfo {
    std::string title;
    std::string build_id;
    u32 latest_version;
};

auto LoadNxDbVersions() -> std::unordered_map<u64, NxDbVersionInfo> {
    std::unordered_map<u64, NxDbVersionInfo> version_map;

    fs::FsNativeSd fs;
    fs::FsPath versions_path;
    std::snprintf(versions_path, sizeof(versions_path), "%s/%s", NX_DB_PATH, NX_DB_VERSIONS_FILE);

    log_write("[Cheats] Loading nx-cheats-db versions from: %s\n", versions_path.s);

    if (!fs.FileExists(versions_path)) {
        log_write("[Cheats] nx-cheats-db versions.json not found\n");
        return version_map;
    }

    std::vector<u8> data;
    Result rc = fs.read_entire_file(versions_path, data);
    if (R_FAILED(rc)) {
        log_write("[Cheats] Failed to read versions.json: %x\n", rc);
        return version_map;
    }

    data.push_back(0); // Null-terminate
    const auto data_len = std::strlen(reinterpret_cast<char*>(data.data()));

    yyjson_doc* doc = yyjson_read(reinterpret_cast<char*>(data.data()), data_len, 0);
    if (!doc) {
        log_write("[Cheats] Failed to parse versions.json\n");
        return version_map;
    }

    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        log_write("[Cheats] versions.json root is not an object\n");
        return version_map;
    }

    // Parse each title entry
    yyjson_val* key;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(root, &iter);

    while ((key = yyjson_obj_iter_next(&iter))) {
        const char* title_id_str = yyjson_get_str(key);
        yyjson_val* title_obj = yyjson_obj_iter_get_val(key);

        if (!title_id_str || !yyjson_is_obj(title_obj)) {
            continue;
        }

        // Parse title ID
        u64 title_id = 0;
        if (sscanf(title_id_str, "%016llx", (unsigned long long*)&title_id) != 1) {
            continue;
        }

        NxDbVersionInfo info;

        // Get title name
        yyjson_val* title_val = yyjson_obj_get(title_obj, "title");
        if (title_val && yyjson_is_str(title_val)) {
            info.title = yyjson_get_str(title_val);
        }

        // Get latest version
        yyjson_val* latest_val = yyjson_obj_get(title_obj, "latest");
        if (latest_val && yyjson_is_uint(latest_val)) {
            info.latest_version = yyjson_get_uint(latest_val);

            // Get build ID for latest version
            std::string version_key = std::to_string(info.latest_version);
            yyjson_val* build_id_val = yyjson_obj_get(title_obj, version_key.c_str());
            if (build_id_val && yyjson_is_str(build_id_val)) {
                info.build_id = yyjson_get_str(build_id_val);
            }
        }

        // If no latest version, try version 0
        if (info.build_id.empty()) {
            yyjson_val* build_id_val = yyjson_obj_get(title_obj, "0");
            if (build_id_val && yyjson_is_str(build_id_val)) {
                info.build_id = yyjson_get_str(build_id_val);
                info.latest_version = 0;
            }
        }

        if (!info.build_id.empty()) {
            version_map[title_id] = std::move(info);
            log_write("[Cheats] nx-cheats-db: %016llx - %s (v%u, %s)\n",
                      (unsigned long long)title_id, info.title.c_str(),
                      info.latest_version, info.build_id.c_str());
        }
    }

    log_write("[Cheats] Loaded %zu titles from nx-cheats-db versions.json\n", version_map.size());
    return version_map;
}

// Check if nx-cheats-db is available
auto IsNxDbAvailable() -> bool {
    fs::FsNativeSd fs;
    fs::FsPath versions_path;
    std::snprintf(versions_path, sizeof(versions_path), "%s/%s", NX_DB_PATH, NX_DB_VERSIONS_FILE);
    return fs.FileExists(versions_path);
}

// Get saved CheatSlips token (checks HATS-Tools and AIO-Switch-Updater paths)
auto GetCheatslipsToken() -> std::string {
    fs::FsNativeSd fs;

    // List of token paths to check (HATS-Tools first, then AIO for compatibility)
    const char* token_paths[] = {TOKEN_PATH, AIO_TOKEN_PATH};

    for (const char* token_path : token_paths) {
        if (!fs.FileExists(token_path)) {
            log_write("[Cheats] Token file not found at %s\n", token_path);
            continue;
        }

        log_write("[Cheats] Found token file at %s\n", token_path);

        std::vector<u8> data;
        Result rc = fs.read_entire_file(token_path, data);
        if (R_FAILED(rc)) {
            log_write("[Cheats] Failed to read token file, result: %x\n", rc);
            continue;
        }

        log_write("[Cheats] Read %zu bytes from token file\n", data.size());

        // Null-terminate for JSON parsing
        data.push_back(0);

        const auto data_len = std::strlen(reinterpret_cast<char*>(data.data()));
        yyjson_doc* doc = yyjson_read((char*)data.data(), data_len, 0);
        if (!doc) {
            log_write("[Cheats] Failed to parse token JSON, raw data: %s\n", (char*)data.data());
            continue;
        }

        ON_SCOPE_EXIT(yyjson_doc_free(doc));

        yyjson_val* root = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(root)) {
            log_write("[Cheats] Token JSON is not an object\n");
            continue;
        }

        yyjson_val* token_val = yyjson_obj_get(root, "token");
        if (token_val && yyjson_is_str(token_val)) {
            const char* token = yyjson_get_str(token_val);
            log_write("[Cheats] Loaded saved token from %s: %s\n", token_path, token);
            // Copy the token string since the doc will be freed
            return std::string(token);
        }

        log_write("[Cheats] No token field in JSON from %s\n", token_path);
    }

    log_write("[Cheats] No valid token found in any location\n");
    return "";
}

// Authenticate with CheatSlips API and get token
auto AuthenticateCheatslips(const std::string& email, const std::string& password) -> std::string {
    // Create JSON body with credentials
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    ON_SCOPE_EXIT(yyjson_mut_doc_free(doc));

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strncpy(doc, root, "email", email.c_str(), email.size());
    yyjson_mut_obj_add_strncpy(doc, root, "password", password.c_str(), password.size());

    char* json_body = yyjson_mut_write(doc, 0, 0);
    if (!json_body) {
        log_write("[Cheats] Failed to create login JSON\n");
        return "";
    }

    ON_SCOPE_EXIT(free(json_body));

    // Send POST request to CheatSlips token endpoint
    // Use ToMemory with Fields for POST (FromMemory adds trailing slash and uses CURLOPT_UPLOAD)
    auto result = curl::Api().ToMemory(
        curl::Url{CHEATSLIPS_TOKEN_URL},
        curl::Header{
            {"Accept", "application/json"},
            {"Content-Type", "application/json"}
        },
        curl::Fields{json_body}
    );

    log_write("[Cheats] Auth HTTP code: %ld\n", result.code);
    if (!result.success || result.data.empty()) {
        log_write("[Cheats] Failed to authenticate with CheatSlips\n");
        return "";
    }

    // Parse response to get token
    result.data.push_back(0); // Null-terminate
    const auto response_len = std::strlen(reinterpret_cast<char*>(result.data.data()));
    yyjson_doc* resp_doc = yyjson_read(reinterpret_cast<char*>(result.data.data()), response_len, 0);
    if (!resp_doc) {
        log_write("[Cheats] Failed to parse auth response\n");
        return "";
    }
    ON_SCOPE_EXIT(yyjson_doc_free(resp_doc));

    yyjson_val* resp_root = yyjson_doc_get_root(resp_doc);
    if (!yyjson_is_obj(resp_root)) {
        return "";
    }

    yyjson_val* token_val = yyjson_obj_get(resp_root, "token");
    if (token_val && yyjson_is_str(token_val)) {
        const char* token = yyjson_get_str(token_val);
        log_write("[Cheats] Authentication successful, token: %s\n", token);
        // Copy the token string since the doc will be freed
        return std::string(token);
    }

    // Check for error message
    yyjson_val* error_val = yyjson_obj_get(resp_root, "error");
    if (error_val && yyjson_is_str(error_val)) {
        log_write("[Cheats] Auth error: %s\n", yyjson_get_str(error_val));
    }

    // Log full response for debugging
    log_write("[Cheats] Auth response: %s\n", reinterpret_cast<char*>(result.data.data()));

    return "";
}

// Save CheatSlips token
auto SaveCheatslipsToken(const std::string& token) -> void {
    fs::FsNativeSd fs;

    // Create directory if needed
    fs.CreateDirectoryRecursively("/config/hats-tools");

    // Create JSON document
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    ON_SCOPE_EXIT(yyjson_mut_doc_free(doc));

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strncpy(doc, root, "token", token.c_str(), token.size());

    // Write to file
    char* json = yyjson_mut_write(doc, 0, 0);
    if (!json) {
        log_write("[Cheats] Failed to write token JSON\n");
        return;
    }

    ON_SCOPE_EXIT(free(json));

    const auto json_data = std::span<const u8>(reinterpret_cast<const u8*>(json), std::strlen(json));
    if (R_FAILED(fs.write_entire_file(TOKEN_PATH, json_data))) {
        log_write("[Cheats] Failed to write token file\n");
        return;
    }

    // Commit to ensure data is written to disk
    if (R_FAILED(fs.Commit())) {
        log_write("[Cheats] Failed to commit token file\n");
        return;
    }

    log_write("[Cheats] Saved CheatSlips token to file, JSON: %s\n", json);
}

// Get the cheats directory path for a title
auto GetCheatsDirPath(u64 title_id) -> std::string {
    const auto title_id_str = FormatTitleIdLower(title_id);
    return std::string(ATMOSPHERE_CONTENTS_PATH) + "/" + title_id_str + "/" + CHEATS_SUBDIR;
}

// Get list of existing cheat files for a title
// Returns map of {build_id: filename}
auto GetExistingCheats(u64 title_id) -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> cheats;
    fs::FsNativeSd fs;

    const auto cheats_dir = GetCheatsDirPath(title_id);
    log_write("[Cheats] Checking for existing cheats in: %s\n", cheats_dir.c_str());

    if (!fs.DirExists(cheats_dir.c_str())) {
        log_write("[Cheats] Cheats directory doesn't exist\n");
        return cheats;
    }

    // Open directory and read entries
    fs::Dir dir;
    if (R_FAILED(fs.OpenDirectory(cheats_dir.c_str(), FsDirOpenMode_ReadFiles, &dir))) {
        log_write("[Cheats] Failed to open cheats directory\n");
        return cheats;
    }

    ON_SCOPE_EXIT(dir.Close());

    s64 count = 0;
    if (R_FAILED(dir.GetEntryCount(&count))) {
        log_write("[Cheats] Failed to get entry count\n");
        return cheats;
    }

    log_write("[Cheats] Found %lld cheat files\n", count);

    std::vector<FsDirectoryEntry> entries(count);
    s64 read_count = 0;
    if (R_FAILED(dir.Read(&read_count, entries.size(), entries.data()))) {
        log_write("[Cheats] Failed to read directory entries\n");
        return cheats;
    }

    for (s64 i = 0; i < read_count; i++) {
        const auto& entry = entries[i];
        if (entry.type == FsDirEntryType_File) {
            // Extract build ID from filename (without .txt extension)
            std::string name = entry.name;
            if (name.length() > 4 && name.substr(name.length() - 4) == ".txt") {
                std::string build_id = name.substr(0, name.length() - 4);
                cheats.push_back({build_id, name});
                log_write("[Cheats] Found cheat: %s (Build ID: %s)\n", name, build_id.c_str());
            }
        }
    }

    return cheats;
}

// Delete a specific cheat file
auto DeleteCheatFile(u64 title_id, const std::string& build_id) -> bool {
    fs::FsNativeSd fs;

    const auto cheats_dir = GetCheatsDirPath(title_id);
    fs::FsPath file_path;
    std::snprintf(file_path, sizeof(file_path), "%s/%s.txt", cheats_dir.c_str(), build_id.c_str());

    if (fs.FileExists(file_path)) {
        Result rc = fs.DeleteFile(file_path);
        if (R_FAILED(rc)) {
            log_write("[Cheats] Failed to delete cheat file %s: %x\n", file_path.s, rc);
            return false;
        }
        log_write("[Cheats] Deleted cheat file: %s\n", file_path.s);
        return true;
    }

    return false;
}

// Delete all cheats for a title
auto DeleteAllCheatsForTitle(u64 title_id) -> bool {
    fs::FsNativeSd fs;

    const auto cheats_dir = GetCheatsDirPath(title_id);

    if (fs.DirExists(cheats_dir.c_str())) {
        Result rc = fs.DeleteDirectoryRecursively(cheats_dir.c_str());
        if (R_FAILED(rc)) {
            log_write("[Cheats] Failed to delete cheats directory %s: %x\n", cheats_dir.c_str(), rc);
            return false;
        }
        log_write("[Cheats] Deleted all cheats for title %016lx\n", title_id);

        // Also try to delete the title directory if empty
        const auto title_dir = std::string(ATMOSPHERE_CONTENTS_PATH) + "/" + FormatTitleIdLower(title_id);
        if (fs.DirExists(title_dir.c_str())) {
            fs.DeleteDirectory(title_dir.c_str());
        }
        return true;
    }

    return false;
}

// Clear cached cheats database from /config/hats-tools/cheats-db
auto ClearCheatsCache() -> Result {
    fs::FsNativeSd fs;

    log_write("[Cheats] Clearing cheats cache: %s\n", NX_DB_PATH);

    if (fs.DirExists(NX_DB_PATH)) {
        Result rc = fs.DeleteDirectoryRecursively(NX_DB_PATH);
        if (R_FAILED(rc)) {
            log_write("[Cheats] Failed to clear cheats cache: %x\n", rc);
            return rc;
        }
        log_write("[Cheats] Successfully cleared cheats cache\n");
        return 0;
    }

    log_write("[Cheats] Cheats cache directory does not exist\n");
    return 0;
}

// Write cheat file to atmosphere/contents/{titleid}/cheats/{buildid}.txt
// Each selected cheat becomes a section in the file
// Now merges with existing cheats instead of overwriting
auto WriteCheatFile(u64 title_id, const std::string& build_id, const std::vector<CheatEntry>& cheats) -> Result {
    fs::FsNativeSd fs;

    // Create cheats directory path: /atmosphere/contents/{titleid}/cheats/
    const auto cheats_dir = GetCheatsDirPath(title_id);
    fs.CreateDirectoryRecursively(cheats_dir.c_str());

    // Create file path: /atmosphere/contents/{titleid}/cheats/{buildid}.txt
    fs::FsPath file_path;
    std::snprintf(file_path, sizeof(file_path), "%s/%s.txt", cheats_dir.c_str(), build_id.c_str());

    log_write("[Cheats] Saving cheats to: %s\n", file_path.s);
    log_write("[Cheats] Build ID: %s, Title ID: %016lx\n", build_id.c_str(), title_id);

    // Parse existing file to get already saved cheats
    std::set<std::string> existing_cheat_names;
    if (fs.FileExists(file_path)) {
        std::vector<u8> existing_data;
        if (R_SUCCEEDED(fs::read_entire_file(&fs, file_path, existing_data))) {
            std::string existing_content(existing_data.begin(), existing_data.end());
            // Parse cheat names from existing content
            std::istringstream stream(existing_content);
            std::string line;
            while (std::getline(stream, line)) {
                // Trim whitespace
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);

                // Check for cheat title bracket format [Title]
                if (!line.empty() && line[0] == '[' && line.back() == ']') {
                    std::string name = line.substr(1, line.length() - 2);
                    existing_cheat_names.insert(name);
                }
            }
            log_write("[Cheats] Found %zu existing cheats in file\n", existing_cheat_names.size());
        }
    }

    // Build cheat file content with source comments
    std::string content;

    // Group cheats by source
    std::map<CheatSource, std::vector<const CheatEntry*>> cheats_by_source;
    for (const auto& cheat : cheats) {
        if (!cheat.selected) continue;
        cheats_by_source[cheat.source].push_back(&cheat);
    }

    // Add cheats with source markers at the END (to preserve source info when reading back)
    for (const auto& [source, source_cheats] : cheats_by_source) {
        // Only process if we have cheats from this source
        if (source_cheats.empty()) continue;

        // Add cheats from this source
        for (const auto* cheat : source_cheats) {
            // Skip if this cheat already exists (by name)
            if (existing_cheat_names.count(cheat->name)) {
                log_write("[Cheats] Skipping duplicate cheat: %s\n", cheat->name.c_str());
                continue;
            }

            // Check if content already starts with [cheat_name]
            // If so, don't duplicate it (nx-cheats-db format already has it)
            std::string content_to_write = cheat->content;
            if (!content_to_write.empty()) {
                // Check if first line is [Name]
                size_t first_newline = content_to_write.find('\n');
                if (first_newline != std::string::npos) {
                    std::string first_line = content_to_write.substr(0, first_newline);
                    // Remove brackets for comparison
                    if (first_line.size() > 2 && first_line[0] == '[' && first_line[first_line.size() - 1] == ']') {
                        std::string first_line_name = first_line.substr(1, first_line.size() - 2);
                        // Check if it matches the cheat name
                        if (first_line_name == cheat->name) {
                            // Content already has [name] prefix, use it as-is
                            content_to_write += "\n";
                        } else {
                            // First line is different, add our prefix
                            content_to_write = "[" + cheat->name + "]\n" + content_to_write + "\n";
                        }
                    } else {
                        // No [name] prefix in content, add it
                        content_to_write = "[" + cheat->name + "]\n" + content_to_write + "\n";
                    }
                } else {
                    // Single line or no newlines, add prefix
                    content_to_write = "[" + cheat->name + "]\n" + content_to_write + "\n";
                }
            }

            content += content_to_write;
            existing_cheat_names.insert(cheat->name); // Mark as added
        }

        content += "\n";
    }

    // Add source footer comment to preserve source information when reading back
    // This is placed at the very end so it doesn't interfere with cheat parsing
    if (!content.empty()) {
        const char* source_footer = nullptr;
        // Use the first source (should typically be one source per download)
        for (const auto& [source, source_cheats] : cheats_by_source) {
            if (!source_cheats.empty()) {
                switch (source) {
                    case CheatSource::Cheatslips:
                        source_footer = "\n// source: CheatSlips\n";
                        break;
                    case CheatSource::NxDb:
                        source_footer = "\n// source: nx-cheats-db\n";
                        break;
                    case CheatSource::Gbatemp:
                        source_footer = "\n// source: GBATemp\n";
                        break;
                }
                break; // Use first source found
            }
        }

        if (source_footer) {
            content += source_footer;
        }
    }

    // If no new cheats to add (all were duplicates)
    if (content.empty()) {
        log_write("[Cheats] No new cheats to add (all duplicates)\n");
        App::Notify("All cheats already exist!");
        return 0;
    }

    // If file exists, append to it; otherwise create new
    const auto content_data = std::span<const u8>(reinterpret_cast<const u8*>(content.data()), content.size());

    if (fs.FileExists(file_path)) {
        // Append to existing file
        FILE* f = fopen(file_path.s, "a");
        if (f) {
            fwrite(content.data(), 1, content.size(), f);
            fclose(f);
            log_write("[Cheats] Appended %zu cheats to %s\n", cheats.size(), file_path.s);
        } else {
            log_write("[Cheats] Failed to open file for appending: %s\n", file_path.s);
            return 1;
        }
    } else {
        // Write new file
        if (R_FAILED(fs.write_entire_file(file_path, content_data))) {
            log_write("[Cheats] Failed to write cheat file %s\n", file_path.s);
            return 1;
        }
        log_write("[Cheats] Wrote %zu cheats to %s\n", cheats.size(), file_path.s);
    }

    return 0;
}

// Delete all cheats for all games
auto DeleteAllCheats() -> Result {
    fs::FsNativeSd fs;

    // Get all installed games first
    std::vector<u64> installed_titles;

    Result rc = ns::Initialize();
    if (R_FAILED(rc)) {
        log_write("[Cheats] ns::Initialize failed: %x\n", rc);
        return rc;
    }

    std::vector<NsApplicationRecord> record_list(ENTRY_CHUNK_COUNT);
    s32 offset = 0;

    while (true) {
        s32 record_count = 0;
        rc = nsListApplicationRecord(record_list.data(), record_list.size(), offset, &record_count);

        if (R_FAILED(rc)) {
            break;
        }

        if (record_count == 0) {
            break;
        }

        for (s32 i = 0; i < record_count; i++) {
            if (record_list[i].application_id != 0) {
                installed_titles.push_back(record_list[i].application_id);
            }
        }

        offset += record_count;
    }

    ns::Exit();

    // Delete cheats for each installed game
    s32 deleted_count = 0;
    for (u64 title_id : installed_titles) {
        if (DeleteAllCheatsForTitle(title_id)) {
            deleted_count++;
        }
    }

    log_write("[Cheats] Deleted cheats for %d games\n", deleted_count);
    return 0;
}

// Delete orphaned cheats (cheats for games that are no longer installed)
auto DeleteOrphanedCheats() -> Result {
    fs::FsNativeSd fs;

    // Get all installed games
    std::vector<u64> installed_titles;

    Result rc = ns::Initialize();
    if (R_FAILED(rc)) {
        log_write("[Cheats] ns::Initialize failed: %x\n", rc);
        return -1;
    }

    std::vector<NsApplicationRecord> record_list(ENTRY_CHUNK_COUNT);
    s32 offset = 0;

    while (true) {
        s32 record_count = 0;
        rc = nsListApplicationRecord(record_list.data(), record_list.size(), offset, &record_count);

        if (R_FAILED(rc)) {
            break;
        }

        if (record_count == 0) {
            break;
        }

        for (s32 i = 0; i < record_count; i++) {
            if (record_list[i].application_id != 0) {
                installed_titles.push_back(record_list[i].application_id);
            }
        }

        offset += record_count;
    }

    ns::Exit();

    log_write("[Cheats] Found %zu installed games\n", installed_titles.size());

    // Scan atmosphere/contents for cheat directories
    s32 deleted_count = 0;

    // Check if atmosphere directory exists
    if (!fs.DirExists(ATMOSPHERE_CONTENTS_PATH)) {
        log_write("[Cheats] Atmosphere contents directory not found\n");
        return 0;
    }

    // Open directory and iterate through subdirectories
    fs::Dir dir;
    if (R_FAILED(fs.OpenDirectory(ATMOSPHERE_CONTENTS_PATH, FsDirOpenMode_ReadDirs, &dir))) {
        log_write("[Cheats] Failed to open atmosphere contents directory\n");
        return -1;
    }

    ON_SCOPE_EXIT(dir.Close());

    s64 count = 0;
    if (R_FAILED(dir.GetEntryCount(&count))) {
        return -1;
    }

    std::vector<FsDirectoryEntry> entries(count);
    s64 read_count = 0;
    if (R_FAILED(dir.Read(&read_count, entries.size(), entries.data()))) {
        return -1;
    }

    for (s64 i = 0; i < read_count; i++) {
        const auto& entry = entries[i];
        if (entry.type != FsDirEntryType_Dir) continue;

        // Parse title ID from directory name
        std::string dir_name = entry.name;
        u64 title_id = 0;
        if (sscanf(dir_name.c_str(), "%016lx", &title_id) != 1) {
            continue;
        }

        // Check if this title is still installed
        bool is_installed = false;
        for (u64 installed : installed_titles) {
            if (installed == title_id) {
                is_installed = true;
                break;
            }
        }

        // If not installed, delete the cheats directory
        if (!is_installed) {
            const auto title_dir = std::string(ATMOSPHERE_CONTENTS_PATH) + "/" + dir_name;
            const auto cheats_dir = title_dir + "/" + CHEATS_SUBDIR;

            if (fs.DirExists(cheats_dir.c_str())) {
                log_write("[Cheats] Deleting orphaned cheats for %016lx\n", title_id);
                if (R_SUCCEEDED(fs.DeleteDirectoryRecursively(cheats_dir.c_str()))) {
                    deleted_count++;
                }

                // Try to delete empty title directory
                fs.DeleteDirectory(title_dir.c_str());
            }
        }
    }

    log_write("[Cheats] Deleted orphaned cheats for %d games\n", deleted_count);
    return deleted_count;
}

} // namespace

// ============================================================
// CheatsMenu - Main menu with cheat management options
// ============================================================

CheatsMenu::CheatsMenu() : MenuBase{"Cheats", MenuFlag_None} {
    // Main cheat management options
    m_items = {
        {"Download Cheats", "from nx-cheats-db (GitHub)"},
        {"Download from CheatSlips", "Online cheat database"},
        {"View Cheats", "View installed cheat codes"},
        {"Delete All Cheats", "Delete all existing cheat codes"},
        {"Delete Orphaned", "Delete cheats for uninstalled games"},
        {"Clear Cheats Cache", "Delete cached cheats database"}
    };

    this->SetActions(
        std::make_pair(Button::A, Action{"Select"_i18n, [this](){
            OnSelect();
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }})
    );

    const Vec4 v{75, GetY() + 42.f, 1220.f - 150.f, 60.f};
    m_list = std::make_unique<List>(1, 8, m_pos, v);
    m_list->SetLayout(List::Layout::GRID);
}

CheatsMenu::~CheatsMenu() {
}

void CheatsMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    m_list->OnUpdate(controller, touch, m_index, m_items.size(), [this](bool touch, auto i) {
        if (touch && m_index == i) {
            FireAction(Button::A);
        } else {
            App::PlaySoundEffect(SoundEffect::Focus);
            SetIndex(i);
        }
    });
}

void CheatsMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    constexpr float text_xoffset{15.f};

    m_list->Draw(vg, theme, m_items.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& item = m_items[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            if (i != m_items.size() - 1) {
                gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
            }
        }

        gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f - 6.f, 20.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            theme->GetColour(text_id),
            "%s", item.first.c_str());

        gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f + 14.f, 14.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "%s", item.second.c_str());
    });
}

void CheatsMenu::OnFocusGained() {
    MenuBase::OnFocusGained();
}

void CheatsMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
}

void CheatsMenu::OnSelect() {
    switch (m_index) {
        case 0: // Download Cheats from nx-cheats-db (default)
            App::Push<CheatGameSelectMenu>(CheatSource::NxDb);
            break;
        case 1: // Download from CheatSlips
            App::Push<CheatGameSelectMenu>(CheatSource::Cheatslips);
            break;
        case 2: // View Cheats
            App::Push<CheatViewMenu>();
            break;
        case 3: // Delete All Cheats
            App::Push<OptionBox>(
                "Delete all existing cheat codes?\nThis will remove ALL cheat files\nfor ALL installed games.",
                "Cancel"_i18n, "Delete", 1,
                [](auto op_index) {
                    if (!op_index || *op_index != 1) {
                        return;
                    }
                    App::Push<ProgressBox>(0, "Deleting..."_i18n, "Cheats"_i18n,
                        [](auto pbox) -> Result {
                            return DeleteAllCheats();
                        },
                        [](Result rc) {
                            if (R_SUCCEEDED(rc)) {
                                App::Notify("Deleted all cheat codes");
                            } else {
                                App::Push<ErrorBox>(rc, "Failed to delete cheats");
                            }
                        }
                    );
                }
            );
            break;
        case 4: // Delete Orphaned Cheats
            App::Push<ProgressBox>(0, "Scanning..."_i18n, "Cheats"_i18n,
                [](auto pbox) -> Result {
                    return DeleteOrphanedCheats();
                },
                [](Result rc) {
                    if (rc == 0) {
                        App::Notify("No orphaned cheats found");
                    } else if (rc > 0) {
                        App::Notify("Deleted " + std::to_string(rc) + " orphaned cheats");
                    } else {
                        App::Push<ErrorBox>(rc, "Failed to delete orphaned cheats");
                    }
                }
            );
            break;
        case 5: // Clear Cheats Cache
            App::Push<OptionBox>(
                "Clear cached cheats database?",
                "Cancel"_i18n, "Clear"_i18n, 0,
                [](auto op_index) {
                    if (!op_index || *op_index != 1) {
                        return;
                    }
                    App::Push<ProgressBox>(0, "Clearing Cache"_i18n, "Cheats"_i18n,
                        [](auto pbox) -> Result {
                            return ClearCheatsCache();
                        },
                        [](Result rc) {
                            if (R_SUCCEEDED(rc)) {
                                App::Notify("Cheats cache cleared successfully");
                            } else {
                                App::Push<ErrorBox>(rc, "Failed to clear cheats cache");
                            }
                        }
                    );
                }
            );
            break;
    }
}

// ============================================================
// CheatViewMenu - View installed cheats
// ============================================================

CheatViewMenu::CheatViewMenu() : MenuBase{"Installed Cheats", MenuFlag_None} {
    this->SetActions(
        std::make_pair(Button::A, Action{"View"_i18n, [this](){
            if (!m_games.empty()) {
                OnSelect();
            }
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::Y, Action{"Delete"_i18n, [this](){
            if (!m_games.empty()) {
                OnDelete();
            }
        }})
    );

    const Vec4 v{75, GetY() + 42.f, 1220.f - 150.f, 60.f};
    m_list = std::make_unique<List>(1, 8, m_pos, v);
    m_list->SetLayout(List::Layout::GRID);
}

CheatViewMenu::~CheatViewMenu() {
}

void CheatViewMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (!m_games.empty()) {
        m_list->OnUpdate(controller, touch, m_index, m_games.size(), [this](bool touch, auto i) {
            if (touch && m_index == i) {
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                SetIndex(i);
            }
        });
    }
}

void CheatViewMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    if (m_scanning) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "Scanning for cheats...");
        return;
    }

    if (m_games.empty() && m_loaded) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "No installed cheats found");
        return;
    }

    if (!m_games.empty()) {
        // Save and restore scissor to clip list drawing area
        nvgSave(vg);
        // Clip area starts below the header text
        nvgScissor(vg, 75.f, GetY() + 40.f, 1220.f - 150.f, 720.f - GetY() - 40.f);
        ON_SCOPE_EXIT(nvgRestore(vg));

        constexpr float text_xoffset{15.f};

        m_list->Draw(vg, theme, m_games.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
            const auto& [x, y, w, h] = v;
            const auto& game = m_games[i];

            auto text_id = ThemeEntryID_TEXT;
            if (m_index == i) {
                text_id = ThemeEntryID_TEXT_SELECTED;
                gfx::drawRectOutline(vg, theme, 4.f, v);
            } else {
                if (i != m_games.size() - 1) {
                    gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
                }
            }

            // Game name
            gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f - 6.f, 20.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
                theme->GetColour(text_id),
                "%s", game.name.c_str());

            // Title ID and cheat count
            gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f + 14.f, 14.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
                theme->GetColour(ThemeEntryID_TEXT_INFO),
                "%016lX - %zu cheat(s)", game.title_id, game.cheat_count);
        });
    }
}

void CheatViewMenu::OnFocusGained() {
    MenuBase::OnFocusGained();

    if (!m_loaded && !m_scanning) {
        ScanGamesWithCheats();
    }
}

void CheatViewMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
}

void CheatViewMenu::OnSelect() {
    if (m_games.empty() || m_index >= (s64)m_games.size()) {
        return;
    }

    const auto& game = m_games[m_index];
    App::Push<CheatFilesMenu>(game);
}

void CheatViewMenu::OnDelete() {
    if (m_games.empty() || m_index >= (s64)m_games.size()) {
        return;
    }

    const auto& game = m_games[m_index];
    App::Push<OptionBox>(
        "Delete all cheats for " + game.name + "?",
        "Cancel"_i18n, "Delete", 1,
        [this, game](auto op_index) {
            if (!op_index || *op_index != 1) {
                return;
            }

            if (DeleteAllCheatsForTitle(game.title_id)) {
                App::Notify("Deleted cheats for " + game.name);
                m_loaded = false; // Rescan
                ScanGamesWithCheats();
            } else {
                App::Notify("Failed to delete cheats");
            }
        }
    );
}

void CheatViewMenu::ScanGamesWithCheats() {
    m_scanning = true;
    m_games.clear();

    // Initialize ns service
    Result rc = ns::Initialize();
    if (R_FAILED(rc)) {
        log_write("[Cheats] ns::Initialize failed: %x\n", rc);
        m_scanning = false;
        m_loaded = true;
        return;
    }

    std::vector<NsApplicationRecord> record_list(ENTRY_CHUNK_COUNT);
    s32 offset = 0;

    while (true) {
        s32 record_count = 0;
        rc = nsListApplicationRecord(record_list.data(), record_list.size(), offset, &record_count);

        if (R_FAILED(rc)) {
            log_write("[Cheats] nsListApplicationRecord failed at offset %d: %x\n", offset, rc);
            break;
        }

        if (record_count == 0) {
            break;
        }

        // Process each record
        for (s32 i = 0; i < record_count; i++) {
            const auto& record = record_list[i];
            if (record.application_id == 0) continue;

            // Check if this game has any cheats
            auto existing = GetExistingCheats(record.application_id);
            if (!existing.empty()) {
                // Get game name
                std::string name = GetTitleName(record.application_id);
                if (name.empty()) {
                    char placeholder[64];
                    std::snprintf(placeholder, sizeof(placeholder), "Game %016llX", (unsigned long long)record.application_id);
                    name = placeholder;
                }

                GameCheatInfo info;
                info.title_id = record.application_id;
                info.name = name;
                info.build_id = "";
                info.version = 0;
                info.cheat_count = existing.size();

                m_games.push_back(std::move(info));
            }
        }

        offset += record_count;
    }

    ns::Exit();

    m_scanning = false;
    m_loaded = true;

    if (!m_games.empty()) {
        SetIndex(0);
    }

    log_write("[Cheats] Total: Found %zu games with cheats\n", m_games.size());
}

// ============================================================
// CheatFilesMenu - View cheat files for a game
// ============================================================

CheatFilesMenu::CheatFilesMenu(const GameCheatInfo& game)
    : MenuBase{"Cheat Files", MenuFlag_None}, m_game(game) {

    // Get existing cheats for this game
    auto existing = GetExistingCheats(game.title_id);
    for (const auto& [build_id, filename] : existing) {
        ExistingCheat cheat;
        cheat.build_id = build_id;
        cheat.filename = filename;
        cheat.installed = true;
        m_cheats.push_back(cheat);
    }

    this->SetActions(
        std::make_pair(Button::A, Action{"View"_i18n, [this](){
            if (!m_cheats.empty()) {
                OnView();
            }
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::X, Action{"Delete"_i18n, [this](){
            if (!m_cheats.empty()) {
                OnDelete();
            }
        }})
    );

    const Vec4 v{75, GetY() + 42.f, 1220.f - 150.f, 60.f};
    m_list = std::make_unique<List>(1, 8, m_pos, v);
    m_list->SetLayout(List::Layout::GRID);
}

CheatFilesMenu::~CheatFilesMenu() {
}

void CheatFilesMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (!m_cheats.empty()) {
        m_list->OnUpdate(controller, touch, m_index, m_cheats.size(), [this](bool touch, auto i) {
            if (touch && m_index == i) {
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                SetIndex(i);
            }
        });
    }
}

void CheatFilesMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    // Draw game info
    gfx::drawTextArgs(vg, 80.f, GetY() + 10.f, 16.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "%s (%016lX)", m_game.name.c_str(), m_game.title_id);

    if (m_cheats.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "No cheat files found");
        return;
    }

    // Save and restore scissor to clip list drawing area
    nvgSave(vg);
    // Clip area starts below the header text
    nvgScissor(vg, 75.f, GetY() + 40.f, 1220.f - 150.f, 720.f - GetY() - 40.f);
    ON_SCOPE_EXIT(nvgRestore(vg));

    constexpr float text_xoffset{15.f};

    m_list->Draw(vg, theme, m_cheats.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& cheat = m_cheats[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            if (i != m_cheats.size() - 1) {
                gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
            }
        }

        gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f, 18.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            theme->GetColour(text_id),
            "Build ID: %s", cheat.build_id.c_str());
    });
}

void CheatFilesMenu::OnFocusGained() {
    MenuBase::OnFocusGained();
}

void CheatFilesMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
}

void CheatFilesMenu::OnView() {
    if (m_cheats.empty() || m_index >= (s64)m_cheats.size()) {
        return;
    }

    const auto& cheat = m_cheats[m_index];
    const auto cheats_dir = GetCheatsDirPath(m_game.title_id);
    fs::FsPath file_path;
    std::snprintf(file_path, sizeof(file_path), "%s/%s.txt", cheats_dir.c_str(), cheat.build_id.c_str());

    fs::FsNativeSd fs;
    std::vector<u8> data;
    if (R_FAILED(fs::read_entire_file(&fs, file_path, data))) {
        App::Notify("Failed to read cheat file");
        return;
    }

    data.push_back(0);
    std::string content(reinterpret_cast<char*>(data.data()));

    // Show cheat content in a proper scrollable view
    App::Push<CheatContentMenu>(m_game, cheat.build_id, content);
}

void CheatFilesMenu::OnDelete() {
    if (m_cheats.empty() || m_index >= (s64)m_cheats.size()) {
        return;
    }

    const auto& cheat = m_cheats[m_index];
    App::Push<OptionBox>(
        "Delete cheat file for Build ID " + cheat.build_id + "?",
        "Cancel"_i18n, "Delete", 1,
        [this, cheat](auto op_index) {
            if (!op_index || *op_index != 1) {
                return;
            }

            if (DeleteCheatFile(m_game.title_id, cheat.build_id)) {
                App::Notify("Deleted cheat file");
                SetPop(); // Go back to refresh the list
            } else {
                App::Notify("Failed to delete cheat file");
            }
        }
    );
}

// ============================================================
// CheatContentMenu - View cheat file content (shows titles in list)
// ============================================================

CheatContentMenu::CheatContentMenu(const GameCheatInfo& game, const std::string& build_id, const std::string& content)
    : MenuBase{"Cheat Content", MenuFlag_None}, m_game(game), m_build_id(build_id) {

    this->SetActions(
        std::make_pair(Button::A, Action{"View Code"_i18n, [this](){
            OnViewCheat();
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }})
    );

    // Parse the cheat content to extract titles
    ParseCheatContent(content);

    const Vec4 v{75, GetY() + 42.f, 1220.f - 150.f, 60.f};
    m_list = std::make_unique<List>(1, 8, m_pos, v);
    m_list->SetLayout(List::Layout::GRID);
}

CheatContentMenu::~CheatContentMenu() {
}

void CheatContentMenu::ParseCheatContent(const std::string& content) {
    m_cheats.clear();

    // First, scan for source footer comment to determine default source
    // Format: // source: CheatSlips or // source: nx-cheats-db
    CheatSource default_source = CheatSource::NxDb; // Default source
    std::string lower_content = content;
    std::transform(lower_content.begin(), lower_content.end(), lower_content.begin(), ::tolower);

    if (lower_content.find("// source: cheatslips") != std::string::npos) {
        default_source = CheatSource::Cheatslips;
    } else if (lower_content.find("// source: nx-cheats-db") != std::string::npos ||
               lower_content.find("// source: nxdb") != std::string::npos) {
        default_source = CheatSource::NxDb;
    } else if (lower_content.find("// source: gbatemp") != std::string::npos) {
        default_source = CheatSource::Gbatemp;
    }

    // Parse cheat file format
    std::istringstream stream(content);
    std::string line;
    CheatTitle current_cheat;
    bool in_cheat = false;
    CheatSource current_source = default_source; // Use detected source

    // Helper function to check if a line should be skipped (non-cheat entries)
    auto should_skip_entry = [](const std::string& name) -> bool {
        if (name.empty()) return true;

        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

        // Skip common non-cheat entries
        if (lower_name.find("www.") != std::string::npos) return true;  // Website URLs (e.g., www.cheatslips.com)
        if (lower_name.find("credits:") == 0) return true;  // Credits entries (e.g., credits: author)
        if (lower_name.find("credit:") == 0) return true;   // Credits entries (e.g., credit: author)
        if (lower_name == "credits") return true;           // Just "Credits"
        if (lower_name == "credit") return true;            // Just "Credit"

        return false;
    };

    while (std::getline(stream, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        // Check for source comment: // cheats from: xxx
        if (!line.empty() && line.substr(0, 2) == "//") {
            std::string lower_line = line;
            std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);

            if (lower_line.find("cheatslips") != std::string::npos) {
                current_source = CheatSource::Cheatslips;
            } else if (lower_line.find("nx-cheats-db") != std::string::npos || lower_line.find("nxdb") != std::string::npos) {
                current_source = CheatSource::NxDb;
            } else if (lower_line.find("gbatemp") != std::string::npos) {
                current_source = CheatSource::Gbatemp;
            }
            continue;
        }

        // Check for cheat title bracket format [Title]
        if (!line.empty() && line[0] == '[' && line.back() == ']') {
            // Save previous cheat if exists
            if (in_cheat && !current_cheat.name.empty()) {
                // Skip non-cheat entries like credits, website headers
                if (!should_skip_entry(current_cheat.name)) {
                    // Determine if cheat is empty (no actual code)
                    std::string content_check = current_cheat.content;
                    size_t start_pos = content_check.find('\n');
                    if (start_pos != std::string::npos) {
                        content_check = content_check.substr(start_pos + 1);
                    }
                    // Check if empty, whitespace only, or contains "Quota exceeded" message
                    std::string content_lower = content_check;
                    std::transform(content_lower.begin(), content_lower.end(), content_lower.begin(), ::tolower);
                    current_cheat.is_empty = content_check.empty() ||
                        content_check.find_first_not_of(" \t\r\n") == std::string::npos ||
                        content_lower.find("quota exceeded") != std::string::npos ||
                        content_lower.find("quotaexceeded") != std::string::npos;

                    // Only add if it's a valid cheat (not empty/whitespace)
                    if (!current_cheat.name.empty() && current_cheat.name.find_first_not_of(" \t\r\n") != std::string::npos) {
                        m_cheats.push_back(current_cheat);
                    }
                }
            }

            // Start new cheat
            current_cheat.name = line.substr(1, line.length() - 2);
            current_cheat.content = line + "\n";
            current_cheat.source = current_source;
            current_cheat.is_empty = false;
            in_cheat = true;
        } else if (in_cheat) {
            // Add line to current cheat content
            current_cheat.content += line + "\n";
        }
    }

    // Don't forget the last cheat
    if (in_cheat && !current_cheat.name.empty()) {
        // Skip non-cheat entries
        if (!should_skip_entry(current_cheat.name)) {
            // Determine if cheat is empty
            std::string content_check = current_cheat.content;
            size_t start_pos = content_check.find('\n');
            if (start_pos != std::string::npos) {
                content_check = content_check.substr(start_pos + 1);
            }
            // Check if empty, whitespace only, or contains "Quota exceeded" message
            std::string content_lower = content_check;
            std::transform(content_lower.begin(), content_lower.end(), content_lower.begin(), ::tolower);
            current_cheat.is_empty = content_check.empty() ||
                content_check.find_first_not_of(" \t\r\n") == std::string::npos ||
                content_lower.find("quota exceeded") != std::string::npos ||
                content_lower.find("quotaexceeded") != std::string::npos;

            // Only add if it's a valid cheat (not empty/whitespace)
            if (!current_cheat.name.empty() && current_cheat.name.find_first_not_of(" \t\r\n") != std::string::npos) {
                m_cheats.push_back(current_cheat);
            }
        }
    }

    log_write("[Cheats] Parsed %zu cheat titles (filtered out non-cheat entries)\n", m_cheats.size());
}

void CheatContentMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (!m_cheats.empty()) {
        m_list->OnUpdate(controller, touch, m_index, m_cheats.size(), [this](bool touch, auto i) {
            if (touch && m_index == i) {
                // On touch, could show full cheat content in a popup
                App::Notify("Press A to view cheat code");
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                SetIndex(i);
            }
        });
    }
}

void CheatContentMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    // Draw header info
    gfx::drawTextArgs(vg, 80.f, GetY() + 10.f, 16.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "%s | %s | %zu cheats", m_game.name.c_str(), m_build_id.c_str(), m_cheats.size());

    if (m_cheats.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "No cheats found");
        return;
    }

    // Save and restore scissor to clip list drawing area
    nvgSave(vg);
    // Clip area starts below the header text
    nvgScissor(vg, 75.f, GetY() + 40.f, 1220.f - 150.f, SCREEN_HEIGHT - 100.f - (GetY() + 40.f));
    ON_SCOPE_EXIT(nvgRestore(vg));

    constexpr float text_xoffset{15.f};

    m_list->Draw(vg, theme, m_cheats.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& cheat = m_cheats[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            if (i != m_cheats.size() - 1) {
                gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
            }
        }

        // Draw source badge
        const char* source_badge = "";
        NVGcolor source_color = theme->GetColour(ThemeEntryID_TEXT_INFO);
        if (cheat.source == CheatSource::Cheatslips) {
            source_badge = "CS";  // Removed brackets to fix rendering
            source_color = nvgRGB(0x4A, 0x90, 0xE2); // Blue for CheatSlips
        } else if (cheat.source == CheatSource::NxDb) {
            source_badge = "NX";  // Removed brackets to fix rendering
            source_color = nvgRGB(0x6B, 0xC6, 0x58); // Green for nx-cheats-db
        } else if (cheat.source == CheatSource::Gbatemp) {
            source_badge = "GB";  // Removed brackets to fix rendering
            source_color = nvgRGB(0xE2, 0x7D, 0x4A); // Orange for GBATemp
        }

        // Cheat name (truncated if too long)
        std::string name = cheat.name;
        if (name.length() > 50) {
            name = name.substr(0, 47) + "...";
        }

        float text_offset = text_xoffset;

        // Draw source badge first (without brackets to avoid rendering issues)
        if (strlen(source_badge) > 0) {
            gfx::drawTextArgs(vg, x + text_offset, y + h / 2.f, 14.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
                source_color,
                "%s", source_badge);
            text_offset += 28.f; // Width of CS/NX/GB text
        }

        // Draw empty indicator if cheat has no content (without brackets)
        if (cheat.is_empty) {
            gfx::drawTextArgs(vg, x + text_offset, y + h / 2.f, 14.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
                nvgRGB(0xFF, 0x00, 0x00), // Red for empty
                "EMPTY");  // Removed brackets to fix rendering
            text_offset += 45.f; // Width of EMPTY text
        }

        // Add extra space before cheat name
        text_offset += 8.f;

        // Draw cheat name with proper spacing
        gfx::drawTextArgs(vg, x + text_offset, y + h / 2.f, 18.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            cheat.is_empty ? nvgRGB(0xFF, 0x66, 0x66) : theme->GetColour(text_id), // Red tint if empty
            "[%s]", name.c_str());
    });
}

void CheatContentMenu::OnFocusGained() {
    MenuBase::OnFocusGained();
}

void CheatContentMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
}

void CheatContentMenu::OnViewCheat() {
    if (m_cheats.empty() || m_index < 0 || m_index >= (s64)m_cheats.size()) {
        return;
    }

    const auto& cheat = m_cheats[m_index];
    App::Push<CheatCodeViewerMenu>(cheat.name, cheat.content, cheat.is_empty);
}

// ============================================================
// CheatCodeViewerMenu - View individual cheat code (scrollable)
// ============================================================

CheatCodeViewerMenu::CheatCodeViewerMenu(const std::string& title, const std::string& content, bool is_empty)
    : MenuBase{"Cheat Code", MenuFlag_None}, m_title(title), m_content(content), m_is_empty(is_empty) {

    this->SetActions(
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }})
    );

    // Calculate content height for scrolling
    // Rough estimation: each line is about 20px tall
    size_t line_count = std::count(m_content.begin(), m_content.end(), '\n') + 1;
    m_content_height = line_count * 20.f;
    if (m_content_height < 100.f) {
        m_content_height = 100.f;
    }
}

CheatCodeViewerMenu::~CheatCodeViewerMenu() {
}

void CheatCodeViewerMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    // Handle scrolling with joystick
    if (controller->GotDown(Button::LS_UP) ||
        controller->GotDown(Button::RS_UP) ||
        controller->GotHeld(Button::LS_UP) ||
        controller->GotHeld(Button::RS_UP)) {
        m_scroll_offset -= 5.f;
    }
    if (controller->GotDown(Button::LS_DOWN) ||
        controller->GotDown(Button::RS_DOWN) ||
        controller->GotHeld(Button::LS_DOWN) ||
        controller->GotHeld(Button::RS_DOWN)) {
        m_scroll_offset += 5.f;
    }

    // Clamp scroll offset
    float max_scroll = m_content_height - (SCREEN_HEIGHT - 150.f);
    if (max_scroll < 0) max_scroll = 0;
    if (m_scroll_offset < 0) m_scroll_offset = 0;
    if (m_scroll_offset > max_scroll) m_scroll_offset = max_scroll;
}

void CheatCodeViewerMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    const float margin = 80.f;
    const float top_margin = GetY() + 50.f;
    const float content_width = SCREEN_WIDTH - 150.f;
    const float max_height = SCREEN_HEIGHT - 150.f;

    // Draw title
    gfx::drawTextArgs(vg, margin, GetY() + 20.f, 20.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_HIGHLIGHT_1),
        "[%s]", m_title.c_str());

    // Draw empty warning if applicable
    if (m_is_empty) {
        gfx::drawTextArgs(vg, margin, GetY() + 120.f, 18.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
            nvgRGB(0xFF, 0x00, 0x00),
            "⚠️ EMPTY CHEAT CODE or QUOTA EXCEEDED!");
    }

    // Save and clip for scrolling
    nvgSave(vg);
    nvgScissor(vg, margin, top_margin, content_width, max_height);

    // Draw cheat code content with scrolling
    float y = top_margin - m_scroll_offset;
    constexpr float line_height = 20.f;

    std::istringstream stream(m_content);
    std::string line;
    while (std::getline(stream, line)) {
        if (y + line_height > top_margin - 20.f && y < top_margin + max_height) {
            // Use monospace-like font for code
            NVGcolor color = theme->GetColour(ThemeEntryID_TEXT);

            // Highlight empty quota message
            std::string line_lower = line;
            std::transform(line_lower.begin(), line_lower.end(), line_lower.begin(), ::tolower);
            if (line_lower.find("quota") != std::string::npos) {
                color = nvgRGB(0xFF, 0x66, 0x66);
            }

            gfx::drawTextArgs(vg, margin, y, 16.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
                color,
                "%s", line.c_str());
        }
        y += line_height;
    }

    nvgRestore(vg);

    // Draw scroll indicator if content is scrollable
    if (m_content_height > max_height) {
        float scroll_bar_height = (max_height / m_content_height) * max_height;
        float scroll_bar_y = top_margin + (m_scroll_offset / m_content_height) * max_height;

        gfx::drawRect(vg, SCREEN_WIDTH - margin + 10.f, scroll_bar_y, 5.f, scroll_bar_height,
            nvgRGBA(0x80, 0x80, 0x80, 0x80));
    }
}

// ============================================================
// CheatGameSelectMenu - Select installed game
// ============================================================

CheatGameSelectMenu::CheatGameSelectMenu(CheatSource source)
    : MenuBase{"Select Game", MenuFlag_None}, m_source(source) {

    // Add logout option for CheatSlips
    if (m_source == CheatSource::Cheatslips) {
        this->SetActions(
            std::make_pair(Button::A, Action{"Select"_i18n, [this](){
                if (!m_games.empty() && !m_scanning) {
                    OnSelect();
                }
            }}),
            std::make_pair(Button::B, Action{"Back"_i18n, [this](){
                SetPop();
            }}),
            std::make_pair(Button::X, Action{"Refresh"_i18n, [this](){
                m_loaded = false;
                ScanGames();
            }}),
            std::make_pair(Button::Y, Action{"Account"_i18n, [this](){
                auto token = GetCheatslipsToken();
                if (!token.empty()) {
                    // Logged in - show logout option
                    App::Push<OptionBox>(
                        "Logged in to CheatSlips.\nLog out?",
                        "Cancel"_i18n, "Logout", 1,
                        [](auto op_index) {
                            if (!op_index || *op_index != 1) {
                                return;
                            }
                            // Delete token file
                            fs::FsNativeSd fs;
                            fs.DeleteFile(TOKEN_PATH);
                            // Also try AIO path
                            fs.DeleteFile(AIO_TOKEN_PATH);
                            App::Notify("Logged out from CheatSlips");
                        }
                    );
                } else {
                    // Logged out - show login option
                    App::Push<OptionBox>(
                        "Not logged in to CheatSlips.\nLog in for higher quotas?",
                        "Cancel"_i18n, "Login", 1,
                        [](auto op_index) {
                            if (!op_index || *op_index != 1) {
                                return;
                            }

                            // Push the login menu (OptionBox will close automatically)
                            App::Push<CheatslipsLoginMenu>();
                        }
                    );
                }
            }})
        );
    } else {
        this->SetActions(
            std::make_pair(Button::A, Action{"Select"_i18n, [this](){
                if (!m_games.empty() && !m_scanning) {
                    OnSelect();
                }
            }}),
            std::make_pair(Button::B, Action{"Back"_i18n, [this](){
                SetPop();
            }}),
            std::make_pair(Button::X, Action{"Refresh"_i18n, [this](){
                m_loaded = false;
                ScanGames();
            }})
        );
    }

    const Vec4 v{75, GetY() + 42.f, 1220.f - 150.f, 60.f};
    m_list = std::make_unique<List>(1, 8, m_pos, v);
    m_list->SetLayout(List::Layout::GRID);
}

CheatGameSelectMenu::~CheatGameSelectMenu() {
}

void CheatGameSelectMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (!m_games.empty()) {
        m_list->OnUpdate(controller, touch, m_index, m_games.size(), [this](bool touch, auto i) {
            if (touch && m_index == i) {
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                SetIndex(i);
            }
        });
    }
}

void CheatGameSelectMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    if (m_scanning) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "Scanning games...");
        return;
    }

    if (m_games.empty() && m_loaded) {
        // Show "no games" message
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f - 20.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "No games found");
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f + 20.f, 18.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "Press X to refresh");
        return;
    }

    if (!m_games.empty()) {
        // Save and restore scissor to clip list drawing area
        nvgSave(vg);
        // Clip area starts below the header text
        nvgScissor(vg, 75.f, GetY() + 40.f, 1220.f - 150.f, 720.f - GetY() - 40.f);
        ON_SCOPE_EXIT(nvgRestore(vg));

        constexpr float text_xoffset{15.f};

        m_list->Draw(vg, theme, m_games.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
            const auto& [x, y, w, h] = v;
            const auto& game = m_games[i];

            auto text_id = ThemeEntryID_TEXT;
            if (m_index == i) {
                text_id = ThemeEntryID_TEXT_SELECTED;
                gfx::drawRectOutline(vg, theme, 4.f, v);
            } else {
                if (i != m_games.size() - 1) {
                    gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
                }
            }

            // Game name
            gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f - 6.f, 20.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
                theme->GetColour(text_id),
                "%s", game.name.c_str());

            // Title ID and version
            gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f + 14.f, 14.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
                theme->GetColour(ThemeEntryID_TEXT_INFO),
                "%016lX v%u", game.title_id, game.version);
        });
    }
}

void CheatGameSelectMenu::OnFocusGained() {
    MenuBase::OnFocusGained();

    if (!m_loaded && !m_scanning) {
        ScanGames();
    }
}

void CheatGameSelectMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
}

void CheatGameSelectMenu::OnSelect() {
    if (m_games.empty() || m_index >= (s64)m_games.size()) {
        return;
    }

    const auto& game = m_games[m_index];

    // For CheatSlips, check if we have a token
    if (m_source == CheatSource::Cheatslips) {
        auto token = GetCheatslipsToken();
        if (token.empty()) {
            // No token, prompt for login (like aio-switch-updater)
            App::Push<OptionBox>(
                "No CheatSlips token found.\nLogin for higher quotas?",
                "Cancel"_i18n, "Login", 1,
                [this, game](auto op_index) {
                    if (!op_index || *op_index != 1) {
                        // User cancelled, proceed without login
                        App::Push<CheatDownloadMenu>(m_source, game);
                        return;
                    }

                    // Push the login menu (OptionBox will close automatically)
                    App::Push<CheatslipsLoginMenu>();
                    // After login, proceed to download
                    App::Push<CheatDownloadMenu>(m_source, game);
                }
            );
            return;
        }
    }

    App::Push<CheatDownloadMenu>(m_source, game);
}

void CheatGameSelectMenu::ScanGames() {
    m_scanning = true;
    m_games.clear();

    // Initialize ns service (like original sphaira game_menu)
    Result rc = ns::Initialize();
    if (R_FAILED(rc)) {
        log_write("[Cheats] ns::Initialize failed: %x\n", rc);
        m_scanning = false;
        m_loaded = true;
        return;
    }

    // Use chunked approach like original sphaira (game_menu.cpp ScanHomebrew)
    std::vector<NsApplicationRecord> record_list(ENTRY_CHUNK_COUNT);
    s32 offset = 0;

    while (true) {
        s32 record_count = 0;
        rc = nsListApplicationRecord(record_list.data(), record_list.size(), offset, &record_count);

        if (R_FAILED(rc)) {
            log_write("[Cheats] nsListApplicationRecord failed at offset %d: %x\n", offset, rc);
            break;
        }

        // Finished parsing all entries
        if (record_count == 0) {
            break;
        }

        log_write("[Cheats] Got %d records at offset %d\n", record_count, offset);

        // Process each record
        for (s32 i = 0; i < record_count; i++) {
            const auto& record = record_list[i];
            if (record.application_id == 0) continue;

            log_write("[Cheats] Processing %016lX\n", record.application_id);

            // Get version
            u32 version = GetTitleVersion(record.application_id);

            // Get title name using nsGetApplicationControlData
            std::string name = GetTitleName(record.application_id);
            if (name.empty()) {
                // Use placeholder name if we couldn't get it
                char placeholder[64];
                std::snprintf(placeholder, sizeof(placeholder), "Game %016llX", (unsigned long long)record.application_id);
                name = placeholder;
            }

            GameCheatInfo info;
            info.title_id = record.application_id;
            info.name = name;
            info.version = version;
            info.build_id = ""; // Will be fetched when needed

            m_games.push_back(std::move(info));
        }

        offset += record_count;
    }

    // Exit ns service when done
    ns::Exit();

    m_scanning = false;
    m_loaded = true;

    if (!m_games.empty()) {
        SetIndex(0);
    }

    log_write("[Cheats] Total: Found %zu games\n", m_games.size());
}

// ============================================================
// CheatDownloadMenu - Select and download cheats
// ============================================================

CheatDownloadMenu::CheatDownloadMenu(CheatSource source, const GameCheatInfo& game)
    : MenuBase{"Select Cheats", MenuFlag_None}, m_source(source), m_game(game) {

    log_write("[Cheats] DEBUG: CheatDownloadMenu constructor called\n");
    log_write("[Cheats] DEBUG: Source: %d, Game: %s, TitleID: %016llX, BuildID: %s\n",
        static_cast<int>(source), game.name.c_str(), game.title_id, game.build_id.c_str());
    log_write("[Cheats] DEBUG: m_should_close initial value: %d\n", m_should_close);

    // Set different actions based on cheat source
    if (m_source == CheatSource::Cheatslips) {
        // CheatSlips: No individual selection (content is bundled), select all and download
        this->SetActions(
            std::make_pair(Button::A, Action{"Download All"_i18n, [this](){
                if (!m_cheats.empty() && !m_loading) {
                    // Select all cheats and download
                    for (auto& cheat : m_cheats) {
                        cheat.selected = true;
                    }
                    DownloadCheats();
                }
            }}),
            std::make_pair(Button::B, Action{"Back"_i18n, [this](){
                SetPop();
            }}),
            std::make_pair(Button::X, Action{"Preview"_i18n, [this](){
                if (!m_cheats.empty() && !m_loading && m_index < (s64)m_cheats.size()) {
                    PreviewCheat();
                }
            }})
        );
    } else {
        // NxDb: Individual selection + Select All + Download + Manage + Preview
        this->SetActions(
            std::make_pair(Button::A, Action{"Toggle"_i18n, [this](){
                if (!m_cheats.empty() && !m_loading) {
                    // Toggle cheat selection
                    if (m_index < (s64)m_cheats.size()) {
                        m_cheats[m_index].selected = !m_cheats[m_index].selected;
                    }
                }
            }}),
            std::make_pair(Button::B, Action{"Back"_i18n, [this](){
                SetPop();
            }}),
            std::make_pair(Button::X, Action{"Select All"_i18n, [this](){
                if (!m_cheats.empty() && !m_loading) {
                    // Select/deselect all cheats
                    bool all_selected = std::all_of(m_cheats.begin(), m_cheats.end(),
                        [](const CheatEntry& c) { return c.selected; });
                    for (auto& cheat : m_cheats) {
                        cheat.selected = !all_selected;
                    }
                }
            }}),
            std::make_pair(Button::Y, Action{"Download"_i18n, [this](){
                if (!m_cheats.empty() && !m_loading) {
                    DownloadCheats();
                }
            }}),
            std::make_pair(Button::R, Action{"Preview"_i18n, [this](){
                if (!m_cheats.empty() && !m_loading && m_index < (s64)m_cheats.size()) {
                    PreviewCheat();
                }
            }})
        );
    }

    const Vec4 v{75, GetY() + 42.f, 1220.f - 150.f, 60.f};
    m_list = std::make_unique<List>(1, 8, m_pos, v);
    m_list->SetLayout(List::Layout::GRID);
}

CheatDownloadMenu::~CheatDownloadMenu() {
    log_write("[Cheats] DEBUG: CheatDownloadMenu destructor called\n");
    log_write("[Cheats] DEBUG: Cheats list size: %zu, m_should_close: %d\n", m_cheats.size(), m_should_close);
}

void CheatDownloadMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    // Check if we should close (from callback)
    if (m_should_close) {
        log_write("[Cheats] DEBUG: Update() - m_should_close flag detected, calling SetPop()\n");
        log_write("[Cheats] DEBUG: Cheats list size: %zu, Index: %lld\n", m_cheats.size(), m_index);
        log_write("[Cheats] DEBUG: Error message: %s\n", m_error_message.c_str());
        SetPop();
        return;
    }

    // Reset index if cheats list is empty
    if (m_cheats.empty()) {
        m_index = -1;
    } else {
        // Ensure index is valid
        if (m_index < 0 || m_index >= (s64)m_cheats.size()) {
            m_index = 0;
        }

        m_list->OnUpdate(controller, touch, m_index, m_cheats.size(), [this](bool touch, auto i) {
            if (touch && m_index == i) {
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                SetIndex(i);
            }
        });
    }
}

void CheatDownloadMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    // Draw game info with Build ID
    if (!m_game.build_id.empty()) {
        gfx::drawTextArgs(vg, 80.f, GetY() + 10.f, 16.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "%s | %s", m_game.name.c_str(), m_game.build_id.c_str());
    } else {
        gfx::drawTextArgs(vg, 80.f, GetY() + 10.f, 16.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "%s", m_game.name.c_str());
    }

    if (m_loading) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "Loading cheats...");
        return;
    }

    if (!m_error_message.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 20.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_ERROR),
            "%s", m_error_message.c_str());
        return;
    }

    if (m_cheats.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "No cheats found");
        return;
    }

    // Save and restore scissor to clip list drawing area
    nvgSave(vg);
    // Clip area starts below the header text
    nvgScissor(vg, 75.f, GetY() + 40.f, 1220.f - 150.f, 720.f - GetY() - 40.f);
    ON_SCOPE_EXIT(nvgRestore(vg));

    constexpr float text_xoffset{15.f};

    m_list->Draw(vg, theme, m_cheats.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& cheat = m_cheats[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            if (i != m_cheats.size() - 1) {
                gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
            }
        }

        // Selection indicator - only for NxDb, not for CheatSlips
        if (m_source != CheatSource::Cheatslips) {
            if (cheat.selected) {
                gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f, 20.f,
                    NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
                    theme->GetColour(ThemeEntryID_HIGHLIGHT_1),
                    "[X]");
            } else {
                gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f, 20.f,
                    NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
                    theme->GetColour(ThemeEntryID_TEXT_INFO),
                    "[ ]");
            }
        }

        // Cheat name (truncated)
        std::string name = cheat.name;
        if (name.length() > 60) {
            name = name.substr(0, 57) + "...";
        }

        // Adjust text offset based on source type
        float text_offset = (m_source == CheatSource::Cheatslips) ? text_xoffset : text_xoffset + 50.f;

        // Draw cheat name (removed source badges - they're only in View Cheats)
        gfx::drawTextArgs(vg, x + text_offset, y + h / 2.f, 18.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            theme->GetColour(text_id),
            "%s", name.c_str());
    });
}

void CheatDownloadMenu::OnFocusGained() {
    MenuBase::OnFocusGained();

    if (!m_loaded && !m_loading) {
        FetchCheats();
    }
}

void CheatDownloadMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
}

void CheatDownloadMenu::FetchCheats() {
    m_loading = true;
    m_error_message.clear();
    m_cheats.clear();

    // For nx-cheats-db, use local database
    if (m_source == CheatSource::NxDb) {
        FetchCheatsFromNxDb();
        return;
    }

    // For CheatSlips, try dmnt:cht service first (fast, requires game to be running)
    log_write("[Cheats] Trying dmnt:cht service first\n");
    std::string build_id = GetBuildIdFromDmnt(m_game.title_id);

    if (!build_id.empty()) {
        // Successfully got Build ID from running game
        m_game.build_id = build_id;
        log_write("[Cheats] Got Build ID from dmnt:cht: %s\n", build_id.c_str());
        FetchCheatsFromApi(build_id);
        return;
    }

    // dmnt:cht failed, try online database
    log_write("[Cheats] dmnt:cht not available, trying online versions database\n");

    // Initialize NS service to get game version
    Result rc = ns::Initialize();
    if (R_FAILED(rc)) {
        log_write("[Cheats] ns::Initialize failed: %x\n", rc);
        m_error_message = "Unable to initialize NS service.\nPlease launch the game first.";
        m_loading = false;
        m_loaded = true;
        return;
    }

    ON_SCOPE_EXIT(ns::Exit);

    u32 version = GetTitleVersion(m_game.title_id);
    if (version == 0) {
        m_error_message = "Unable to determine game version.\nPlease launch the game first.";
        m_loading = false;
        m_loaded = true;
        return;
    }

    const auto title_id_str = FormatTitleId(m_game.title_id);
    const auto versions_url = std::string(VERSIONS_DB_URL) + "/" + title_id_str + ".json";

    log_write("[Cheats] Fetching versions from: %s\n", versions_url.c_str());

    // Fetch the versions file asynchronously
    curl::Api().ToMemoryAsync(
        curl::Url{versions_url},
        curl::Header{},
        curl::StopToken{this->GetToken()},
        curl::OnComplete{[this, version](auto& result) {
            m_loading = false;
            m_loaded = true;
            m_index = -1; // Reset index

            // Check for HTTP 404 (Not Found) - immediately notify user
            if (result.code == 404) {
                m_cheats.clear();
                m_error_message = "Game not found in switch-cheats-db.\nPlease launch the game first.";
                log_write("[Cheats] Game not found in switch-cheats-db (HTTP 404)\n");
                App::Notify("Game not found in database");
                SetPop();
                return true;
            }

            if (!result.success) {
                m_cheats.clear();
                m_error_message = "Failed to fetch Build ID from online database.\nPlease launch the game first.\nCheck your internet connection.";
                log_write("[Cheats] Failed to fetch versions DB, HTTP code: %ld\n", result.code);
                App::Notify("Failed to fetch from database");
                SetPop();
                return true;
            }

            std::string content(result.data.begin(), result.data.end());
            log_write("[Cheats] Versions DB response size: %zu bytes\n", content.size());

            // Parse the versions JSON
            yyjson_doc* doc = yyjson_read(content.data(), content.size(), 0);
            if (!doc) {
                m_error_message = "Failed to parse versions database.";
                log_write("[Cheats] Failed to parse versions database\n");
                App::Notify("Failed to parse database");
                SetPop();
                return true;
            }

            ON_SCOPE_EXIT(yyjson_doc_free(doc));

            yyjson_val* root = yyjson_doc_get_root(doc);
            if (!yyjson_is_obj(root)) {
                m_error_message = "Invalid versions database format.";
                log_write("[Cheats] Invalid versions database format\n");
                App::Notify("Invalid database format");
                SetPop();
                return true;
            }

            std::string build_id;

            // Try exact version match first
            const std::string version_key = std::to_string(version);
            yyjson_val* build_id_val = yyjson_obj_get(root, version_key.c_str());

            if (build_id_val && yyjson_is_str(build_id_val)) {
                build_id = yyjson_get_str(build_id_val);
                log_write("[Cheats] Found Build ID for version %u: %s\n", version, build_id.c_str());
            } else {
                // Fall back to latest version
                log_write("[Cheats] Version %u not found, using latest version\n", version);

                std::string latest_build_id;
                u32 latest_version = 0;

                log_write("[Cheats] DEBUG: Starting JSON iteration to find latest version\n");
                yyjson_val* key;
                yyjson_obj_iter iter;
                yyjson_obj_iter_init(root, &iter);
                size_t iter_count = 0;
                while ((key = yyjson_obj_iter_next(&iter))) {
                    iter_count++;
                    const char* version_str = yyjson_get_str(key);
                    yyjson_val* bid_val = yyjson_obj_iter_get_val(key);

                    log_write("[Cheats] DEBUG: Iteration %zu: version_str=%s\n", iter_count,
                        version_str ? version_str : "(null)");

                    if (version_str && yyjson_is_str(bid_val)) {
                        // Manual validation instead of exceptions (Switch devkit has -fno-exceptions)
                        bool is_valid = true;
                        for (const char* p = version_str; *p; p++) {
                            if (*p < '0' || *p > '9') {
                                is_valid = false;
                                log_write("[Cheats] DEBUG: Invalid version string '%s' contains non-digit character\n", version_str);
                                break;
                            }
                        }

                        if (is_valid && iter_count < 1000) { // Safety limit
                            u32 ver = std::stoul(version_str);
                            log_write("[Cheats] DEBUG: Parsed version %u\n", ver);
                            if (ver > latest_version) {
                                latest_version = ver;
                                latest_build_id = yyjson_get_str(bid_val);
                                log_write("[Cheats] DEBUG: New latest version: %u, Build ID: %s\n",
                                    latest_version, latest_build_id.c_str());
                            }
                        }
                    }
                }
                log_write("[Cheats] DEBUG: JSON iteration complete, checked %zu entries\n", iter_count);

                build_id = latest_build_id;
                if (!build_id.empty()) {
                    log_write("[Cheats] Using latest version %u Build ID: %s\n", latest_version, build_id.c_str());
                } else {
                    log_write("[Cheats] DEBUG: latest_build_id is empty after iteration!\n");
                }
            }

            if (build_id.empty()) {
                m_error_message = "Build ID not found in database.\nPlease launch the game first.";
                log_write("[Cheats] Build ID not found in database\n");
                App::Notify("Build ID not found");
                SetPop();
                return true;
            }

            // Store the build ID and fetch cheats
            m_game.build_id = build_id;
            FetchCheatsFromApi(build_id);
            return true;
        }}
    );
}

// Fetch cheats from nx-cheats-db on GitHub
void CheatDownloadMenu::FetchCheatsFromNxDb() {
    log_write("[Cheats] Fetching cheats from nx-cheats-db (GitHub)\n");

    // First try dmnt:cht service (fast, requires game to be running)
    std::string build_id = GetBuildIdFromDmnt(m_game.title_id);

    if (!build_id.empty()) {
        // Successfully got Build ID from running game
        m_game.build_id = build_id;
        log_write("[Cheats] Got Build ID from dmnt:cht: %s\n", build_id.c_str());
        // Fetch cheats directly
        FetchNxDbCheatsFromGithub(build_id);
        return;
    }

    // Need to get build ID from versions.json on GitHub
    log_write("[Cheats] dmnt:cht not available, fetching versions.json from GitHub\n");

    const auto title_id_str = FormatTitleId(m_game.title_id);
    const auto versions_url = std::string(NX_DB_VERSIONS_URL);

    log_write("[Cheats] Fetching versions from: %s\n", versions_url.c_str());

    // Fetch versions.json to find build ID
    curl::Api().ToMemoryAsync(
        curl::Url{versions_url},
        curl::Header{},
        curl::StopToken{this->GetToken()},
        curl::OnComplete{[this](auto& result) {
            m_loading = false;
            m_loaded = true;

            if (!result.success) {
                m_error_message = "Failed to fetch versions.json from nx-cheats-db.\nCheck your internet connection.";
                log_write("[Cheats] Failed to fetch versions.json, HTTP code: %ld\n", result.code);
                return true;
            }

            std::string content(result.data.begin(), result.data.end());
            log_write("[Cheats] versions.json response size: %zu bytes\n", content.size());

            // Parse versions.json to find build ID for this title
            yyjson_doc* doc = yyjson_read(content.data(), content.size(), 0);
            if (!doc) {
                m_error_message = "Failed to parse versions.json";
                log_write("[Cheats] Failed to parse versions.json\n");
                return true;
            }

            ON_SCOPE_EXIT(yyjson_doc_free(doc));

            yyjson_val* root = yyjson_doc_get_root(doc);
            if (!yyjson_is_obj(root)) {
                m_error_message = "Invalid versions.json format";
                log_write("[Cheats] Invalid versions.json format\n");
                return true;
            }

            // Look up title ID
            char title_id_key[17];
            std::snprintf(title_id_key, sizeof(title_id_key), "%016llX", (unsigned long long)m_game.title_id);

            yyjson_val* title_obj = yyjson_obj_get(root, title_id_key);
            if (!title_obj || !yyjson_is_obj(title_obj)) {
                m_error_message = "Title not found in nx-cheats-db.\nTitle ID: " + std::string(title_id_key);
                log_write("[Cheats] Title %s not found in versions.json\n", title_id_key);
                return true;
            }

            // Get latest version's build ID
            std::string build_id;
            yyjson_val* latest_val = yyjson_obj_get(title_obj, "latest");
            if (latest_val && yyjson_is_uint(latest_val)) {
                u32 latest = yyjson_get_uint(latest_val);
                std::string version_key = std::to_string(latest);
                yyjson_val* build_id_val = yyjson_obj_get(title_obj, version_key.c_str());
                if (build_id_val && yyjson_is_str(build_id_val)) {
                    build_id = yyjson_get_str(build_id_val);
                }
            }

            // Fall back to version 0
            if (build_id.empty()) {
                yyjson_val* build_id_val = yyjson_obj_get(title_obj, "0");
                if (build_id_val && yyjson_is_str(build_id_val)) {
                    build_id = yyjson_get_str(build_id_val);
                }
            }

            if (build_id.empty()) {
                m_error_message = "No Build ID found for this title";
                log_write("[Cheats] No Build ID found for title %s\n", title_id_key);
                return true;
            }

            m_game.build_id = build_id;
            log_write("[Cheats] Found Build ID from versions.json: %s\n", build_id.c_str());

            // Now fetch the actual cheats
            FetchNxDbCheatsFromGithub(build_id);
            return true;
        }}
    );
}

// Fetch cheat JSON file from GitHub
void CheatDownloadMenu::FetchNxDbCheatsFromGithub(const std::string& build_id) {
    const auto title_id_str = FormatTitleId(m_game.title_id);  // UPPERCASE for nx-cheats-db
    const auto cheat_url = std::string(NX_DB_GITHUB_BASE) + "/cheats/" + title_id_str + ".json";

    log_write("[Cheats] Fetching cheats from: %s\n", cheat_url.c_str());

    curl::Api().ToMemoryAsync(
        curl::Url{cheat_url},
        curl::Header{},
        curl::StopToken{this->GetToken()},
        curl::OnComplete{[this, build_id](auto& result) {
            m_loading = false;
            m_loaded = true;
            m_index = -1; // Reset index

            // Check for HTTP 404 (Not Found) - immediately notify user
            if (result.code == 404) {
                m_cheats.clear();
                m_error_message = "Game not found in nx-cheats-db.\nTitle ID: " + FormatTitleId(m_game.title_id);
                log_write("[Cheats] Game not found in nx-cheats-db (HTTP 404)\n");
                App::Notify("Game not found in nx-cheats-db");
                SetPop();
                return true;
            }

            if (!result.success) {
                m_cheats.clear();
                m_error_message = "Failed to fetch cheats from nx-cheats-db.\nTitle may not be supported.\nCheck your internet connection.";
                log_write("[Cheats] Failed to fetch cheat file, HTTP code: %ld\n", result.code);
                App::Notify("Failed to fetch from nx-cheats-db");
                SetPop();
                return true;
            }

            std::string content(result.data.begin(), result.data.end());
            log_write("[Cheats] Cheat file response size: %zu bytes\n", content.size());

            // Parse the cheat JSON
            m_cheats = ParseNxDbCheats(content, build_id);

            if (m_cheats.empty()) {
                m_error_message = "No cheats found for this title.\nBuild ID: " + build_id;
                log_write("[Cheats] No cheats found for title with Build ID: %s\n", build_id.c_str());
                App::Notify("No cheats found for this Build ID");
                SetPop();
            } else {
                m_index = 0; // Set to first item when cheats are found
                log_write("[Cheats] Successfully fetched %zu cheats from nx-cheats-db\n", m_cheats.size());
                // Optionally cache the cheat file locally
                CacheNxDbCheatFile(content);
            }

            return true;
        }}
    );
}

// Cache the fetched cheat file locally for offline use
void CheatDownloadMenu::CacheNxDbCheatFile(const std::string& content) {
    fs::FsNativeSd fs;

    // Create cache directory
    fs.CreateDirectoryRecursively(NX_DB_PATH);

    // Write cheat file (use UPPERCASE for nx-cheats-db)
    fs::FsPath cache_path;
    const auto title_id_str = FormatTitleId(m_game.title_id);  // UPPERCASE for nx-cheats-db
    std::snprintf(cache_path, sizeof(cache_path), "%s/%s.json", NX_DB_PATH, title_id_str.c_str());

    const auto content_data = std::span<const u8>(reinterpret_cast<const u8*>(content.data()), content.size());
    if (R_SUCCEEDED(fs.write_entire_file(cache_path, content_data))) {
        log_write("[Cheats] Cached cheat file to: %s\n", cache_path.s);
    }
}

void CheatDownloadMenu::FetchCheatsFromApi(const std::string& build_id) {
    // Get token (optional - API works without it but has lower quota)
    auto token = GetCheatslipsToken();

    const auto title_id_str = FormatTitleId(m_game.title_id);
    // CheatSlips API URL: /api/v1/cheats/{title_id} (build_id is for filtering, not in URL)
    const auto url = std::string(CHEATSLIPS_API_URL) + "/" + title_id_str;

    log_write("[Cheats] Fetching cheats from CheatSlips: %s\n", url.c_str());

    // Prepare headers - token is optional
    if (!token.empty()) {
        log_write("[Cheats] Using authenticated request (higher quota)\n");
        curl::Api().ToMemoryAsync(
            curl::Url{url},
            curl::Header{
                {"Accept", "application/json"},
                {"X-API-TOKEN", token}
            },
            curl::StopToken{this->GetToken()},
            curl::OnComplete{[this, build_id](auto& result) {
                log_write("[Cheats] DEBUG: CheatSlips AUTH callback triggered\n");
                log_write("[Cheats] CheatSlips request completed - success: %d, HTTP code: %ld\n", result.success, result.code);
                log_write("[Cheats] DEBUG: Response data size: %zu bytes\n", result.data.size());

                m_loading = false;
                m_loaded = true;
                m_index = -1; // Reset index when loading completes

                // Check for HTTP 404 (Not Found) - immediately notify user
                if (result.code == 404) {
                    log_write("[Cheats] DEBUG: HTTP 404 detected (AUTH) - Game not found on CheatSlips\n");
                    m_cheats.clear();
                    m_index = -1;
                    m_error_message = "Game not found on CheatSlips.\nTitle ID: " + FormatTitleId(m_game.title_id);
                    log_write("[Cheats] Game not found on CheatSlips (HTTP 404)\n");
                    log_write("[Cheats] DEBUG: Setting m_should_close = true (404 case)\n");
                    App::Notify("Game not found on CheatSlips");
                    m_should_close = true;
                    log_write("[Cheats] DEBUG: m_should_close set to: %d\n", m_should_close);
                    return true;
                }

                if (!result.success) {
                    log_write("[Cheats] DEBUG: Request failed (AUTH) - success=false, HTTP code: %ld\n", result.code);
                    m_cheats.clear();
                    m_index = -1;
                    m_error_message = "Failed to fetch cheats from CheatSlips.\nCheck your internet connection.";
                    log_write("[Cheats] Failed to fetch CheatSlips cheats, HTTP code: %ld\n", result.code);
                    log_write("[Cheats] DEBUG: Setting m_should_close = true (failure case)\n");
                    // Auto-exit with notification
                    App::Notify("Failed to fetch from CheatSlips");
                    m_should_close = true;
                    log_write("[Cheats] DEBUG: m_should_close set to: %d\n", m_should_close);
                    return true;
                }

                std::string content(result.data.begin(), result.data.end());
                log_write("[Cheats] CheatSlips response size: %zu bytes\n", content.size());
                log_write("[Cheats] DEBUG: Response content preview (first 200 chars): %s\n",
                    content.substr(0, std::min(size_t(200), content.size())).c_str());

                // Check if response is empty or just "[]"
                if (content.empty() || content == "[]" || content == "null") {
                    log_write("[Cheats] DEBUG: Empty response detected (AUTH) - content: '%s'\n",
                        content.empty() ? "(empty)" : content.c_str());
                    m_cheats.clear();
                    m_index = -1;
                    m_error_message = "No cheats found for Build ID: " + build_id + "\nThis game may not be supported on CheatSlips.";
                    log_write("[Cheats] Empty response from CheatSlips\n");
                    log_write("[Cheats] DEBUG: Setting m_should_close = true (empty response case)\n");
                    App::Notify("No cheats found on CheatSlips for this game");
                    m_should_close = true;
                    log_write("[Cheats] DEBUG: m_should_close set to: %d\n", m_should_close);
                    return true;
                }

                log_write("[Cheats] DEBUG: Parsing CheatSlips response (AUTH)...\n");
                m_cheats = ParseCheatslipsCheats(content, build_id);
                log_write("[Cheats] DEBUG: Parsing complete, cheats count: %zu\n", m_cheats.size());

                if (m_cheats.empty()) {
                    log_write("[Cheats] DEBUG: Parsed cheats list is empty (AUTH)\n");
                    // Check if response contains quota error
                    if (content.find("Quota exceeded") != std::string::npos ||
                        content.find("quota") != std::string::npos) {
                        m_error_message = "Daily quota exceeded.\nAdd a token for higher limits.";
                        App::Notify("Daily quota exceeded - Add token for higher limits");
                    } else {
                        m_error_message = "No cheats found for Build ID: " + build_id + "\nThis game may not be supported on CheatSlips.";
                        App::Notify("No cheats found on CheatSlips for this game");
                    }
                    log_write("[Cheats] No cheats found, error: %s\n", m_error_message.c_str());
                    log_write("[Cheats] DEBUG: Setting m_should_close = true (no cheats case)\n");
                    // Auto-exit
                    m_should_close = true;
                    log_write("[Cheats] DEBUG: m_should_close set to: %d\n", m_should_close);
                } else {
                    m_index = 0; // Set to first item when cheats are found
                    log_write("[Cheats] Successfully fetched %zu cheats\n", m_cheats.size());
                    log_write("[Cheats] DEBUG: Leaving m_should_close = false (success case)\n");
                }

                return true;
            }}
        );
    } else {
        log_write("[Cheats] Using unauthenticated request (limited quota)\n");
        curl::Api().ToMemoryAsync(
            curl::Url{url},
            curl::Header{
                {"Accept", "application/json"}
            },
            curl::StopToken{this->GetToken()},
            curl::OnComplete{[this, build_id](auto& result) {
                log_write("[Cheats] DEBUG: CheatSlips NO-AUTH callback triggered\n");
                log_write("[Cheats] CheatSlips request completed - success: %d, HTTP code: %ld\n", result.success, result.code);
                log_write("[Cheats] DEBUG: Response data size: %zu bytes\n", result.data.size());

                m_loading = false;
                m_loaded = true;
                m_index = -1; // Reset index when loading completes

                // Check for HTTP 404 (Not Found) - immediately notify user
                if (result.code == 404) {
                    log_write("[Cheats] DEBUG: HTTP 404 detected (NO-AUTH) - Game not found on CheatSlips\n");
                    m_cheats.clear();
                    m_index = -1;
                    m_error_message = "Game not found on CheatSlips.\nTitle ID: " + FormatTitleId(m_game.title_id);
                    log_write("[Cheats] Game not found on CheatSlips (HTTP 404)\n");
                    log_write("[Cheats] DEBUG: Setting m_should_close = true (404 case)\n");
                    App::Notify("Game not found on CheatSlips");
                    m_should_close = true;
                    log_write("[Cheats] DEBUG: m_should_close set to: %d\n", m_should_close);
                    return true;
                }

                if (!result.success) {
                    log_write("[Cheats] DEBUG: Request failed (NO-AUTH) - success=false, HTTP code: %ld\n", result.code);
                    m_cheats.clear();
                    m_index = -1;
                    m_error_message = "Failed to fetch cheats from CheatSlips.\nCheck your internet connection.";
                    log_write("[Cheats] Failed to fetch CheatSlips cheats, HTTP code: %ld\n", result.code);
                    log_write("[Cheats] DEBUG: Setting m_should_close = true (failure case)\n");
                    // Auto-exit with notification
                    App::Notify("Failed to fetch from CheatSlips");
                    m_should_close = true;
                    log_write("[Cheats] DEBUG: m_should_close set to: %d\n", m_should_close);
                    return true;
                }

                std::string content(result.data.begin(), result.data.end());
                log_write("[Cheats] CheatSlips response size: %zu bytes\n", content.size());
                log_write("[Cheats] DEBUG: Response content preview (first 200 chars): %s\n",
                    content.substr(0, std::min(size_t(200), content.size())).c_str());

                // Check if response is empty or just "[]"
                if (content.empty() || content == "[]" || content == "null") {
                    log_write("[Cheats] DEBUG: Empty response detected (NO-AUTH) - content: '%s'\n",
                        content.empty() ? "(empty)" : content.c_str());
                    m_cheats.clear();
                    m_index = -1;
                    m_error_message = "No cheats found for Build ID: " + build_id + "\nThis game may not be supported on CheatSlips.";
                    log_write("[Cheats] Empty response from CheatSlips\n");
                    log_write("[Cheats] DEBUG: Setting m_should_close = true (empty response case)\n");
                    App::Notify("No cheats found on CheatSlips for this game");
                    m_should_close = true;
                    log_write("[Cheats] DEBUG: m_should_close set to: %d\n", m_should_close);
                    return true;
                }

                log_write("[Cheats] DEBUG: Parsing CheatSlips response (NO-AUTH)...\n");
                m_cheats = ParseCheatslipsCheats(content, build_id);
                log_write("[Cheats] DEBUG: Parsing complete, cheats count: %zu\n", m_cheats.size());

                if (m_cheats.empty()) {
                    log_write("[Cheats] DEBUG: Parsed cheats list is empty (NO-AUTH)\n");
                    // Check if response contains quota error
                    if (content.find("Quota exceeded") != std::string::npos ||
                        content.find("quota") != std::string::npos) {
                        m_error_message = "Daily quota exceeded.\nAdd a token for higher limits.";
                        App::Notify("Daily quota exceeded - Add token for higher limits");
                    } else {
                        m_error_message = "No cheats found for Build ID: " + build_id + "\nThis game may not be supported on CheatSlips.";
                        App::Notify("No cheats found on CheatSlips for this game");
                    }
                    log_write("[Cheats] No cheats found, error: %s\n", m_error_message.c_str());
                    log_write("[Cheats] DEBUG: Setting m_should_close = true (no cheats case)\n");
                    // Auto-exit
                    m_should_close = true;
                    log_write("[Cheats] DEBUG: m_should_close set to: %d\n", m_should_close);
                } else {
                    m_index = 0; // Set to first item when cheats are found
                    log_write("[Cheats] Successfully fetched %zu cheats\n", m_cheats.size());
                    log_write("[Cheats] DEBUG: Leaving m_should_close = false (success case)\n");
                }

                return true;
            }}
        );
    }
}

void CheatDownloadMenu::DownloadCheats() {
    // Check if we have a valid build ID
    if (m_game.build_id.empty()) {
        App::Notify("No Build ID detected!");
        return;
    }

    // Check if cheats list is empty or still loading
    if (m_loading) {
        App::Notify("Still loading cheats, please wait...");
        return;
    }

    if (m_cheats.empty()) {
        App::Notify("No cheats available to download!");
        return;
    }

    // Count selected cheats
    size_t selected_count = 0;
    for (const auto& cheat : m_cheats) {
        if (cheat.selected) selected_count++;
    }

    if (selected_count == 0) {
        App::Notify("No cheats selected!");
        return;
    }

    App::Push<OptionBox>(
        "Download " + std::to_string(selected_count) + " cheat(s)?",
        "Cancel"_i18n, "Download", 1,
        [this](auto op_index) {
            if (!op_index || *op_index != 1) {
                return;
            }

            App::Push<ProgressBox>(0, "Downloading"_i18n, m_game.name,
                [this](auto pbox) -> Result {
                    // Collect selected cheats
                    std::vector<CheatEntry> selected;
                    for (const auto& cheat : m_cheats) {
                        if (cheat.selected) {
                            selected.push_back(cheat);
                        }
                    }

                    return WriteCheatFile(m_game.title_id, m_game.build_id, selected);
                },
                [this](Result rc) {
                    if (R_SUCCEEDED(rc)) {
                        // Count actually downloaded cheats
                        size_t count = 0;
                        for (const auto& cheat : m_cheats) {
                            if (cheat.selected) count++;
                        }
                        App::Notify("Downloaded " + std::to_string(count) + " cheat(s) for " + m_game.name);
                        SetPop();
                    } else {
                        App::Push<ErrorBox>(rc, "Failed to download cheats");
                    }
                }
            );
        }
    );
}

void CheatDownloadMenu::DeleteCheat() {
    if (m_game.build_id.empty()) {
        App::Notify("No Build ID detected!");
        return;
    }

    App::Push<OptionBox>(
        "Delete cheat file for Build ID " + m_game.build_id + "?",
        "Cancel"_i18n, "Delete", 1,
        [this](auto op_index) {
            if (!op_index || *op_index != 1) {
                return;
            }

            if (DeleteCheatFile(m_game.title_id, m_game.build_id)) {
                App::Notify("Deleted cheat file");
            } else {
                App::Notify("Cheat file not found");
            }
        }
    );
}

void CheatDownloadMenu::ShowExistingCheats() {
    m_existing_cheats.clear();

    auto existing = GetExistingCheats(m_game.title_id);
    for (const auto& [build_id, filename] : existing) {
        ExistingCheat cheat;
        cheat.build_id = build_id;
        cheat.filename = filename;
        cheat.installed = (build_id == m_game.build_id);
        m_existing_cheats.push_back(cheat);
    }

    if (m_existing_cheats.empty()) {
        App::Notify("No existing cheats found");
        return;
    }

    // Build a message showing existing cheats
    std::string msg = "Existing cheat files:\n\n";
    for (const auto& cheat : m_existing_cheats) {
        msg += cheat.build_id;
        if (cheat.build_id == m_game.build_id) {
            msg += " [Current]";
        }
        msg += "\n";
    }

    App::Push<OptionBox>(msg, "Close"_i18n, "", 0, [](auto) {});
}

void CheatDownloadMenu::PreviewCheat() {
    if (m_index < 0 || m_index >= (s64)m_cheats.size()) {
        return;
    }

    const auto& cheat = m_cheats[m_index];

    // Build preview message
    std::string msg = "Cheat Preview:\n\n";
    msg += "Name: " + cheat.name + "\n";
    msg += "Build ID: " + cheat.build_id + "\n";

    // Add source info
    const char* source_str = "Unknown";
    switch (cheat.source) {
        case CheatSource::Cheatslips:
            source_str = "CheatSlips";
            break;
        case CheatSource::NxDb:
            source_str = "nx-cheats-db";
            break;
        case CheatSource::Gbatemp:
            source_str = "GBATemp";
            break;
    }
    msg += "Source: " + std::string(source_str) + "\n\n";

    // Add content
    msg += "Content:\n";
    msg += "─────────────────────────────\n";

    // Check if content is empty or just whitespace
    std::string content = cheat.content;
    if (content.empty() || content.find_first_not_of(" \t\r\n") == std::string::npos) {
        msg += "[BLANK OR QUOTA EXCEEDED]\n";
        msg += "\n⚠️ WARNING: This cheat has no content!\n";
        msg += "This can happen when CheatSlips quota is exceeded.\n";
    } else {
        // Truncate content if too long for display (max ~500 chars)
        if (content.length() > 500) {
            content = content.substr(0, 497) + "...";
        }
        msg += content;
    }

    msg += "\n─────────────────────────────";

    // Show preview dialog
    App::Push<OptionBox>(msg, "Close"_i18n, "", 0, [](auto) {});
}

// CheatslipsLoginMenu implementation
CheatslipsLoginMenu::CheatslipsLoginMenu()
    : MenuBase{"CheatSlips Login", MenuFlag_None} {
    this->SetActions(
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }})
    );
}

CheatslipsLoginMenu::~CheatslipsLoginMenu() = default;

void CheatslipsLoginMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);
}

void CheatslipsLoginMenu::Draw(NVGcontext* vg, Theme* theme) {
    // Don't draw anything - keyboard provides the UI
    (void)vg;
    (void)theme;
}

void CheatslipsLoginMenu::OnFocusGained() {
    m_keyboard_shown = false;
    ShowEmailKeyboard();
}

void CheatslipsLoginMenu::ShowEmailKeyboard() {
    if (m_keyboard_shown) return;
    m_keyboard_shown = true;

    std::string email;
    Result rc = swkbd::ShowText(email, "CheatSlips Email", "Enter your email", "", 0, 32);
    if (R_FAILED(rc) || email.empty()) {
        SetPop();
        return;
    }

    m_email = email;
    m_state = LoginState::Password;
    m_keyboard_shown = false;
    ShowPasswordKeyboard();
}

void CheatslipsLoginMenu::ShowPasswordKeyboard() {
    if (m_keyboard_shown) return;
    m_keyboard_shown = true;

    std::string password;
    Result rc = swkbd::ShowPassword(password, "CheatSlips Password", "Enter your password", "", 0, 32);
    if (R_FAILED(rc) || password.empty()) {
        SetPop();
        return;
    }

    m_password = password;
    Authenticate();
}

void CheatslipsLoginMenu::Authenticate() {
    auto token = AuthenticateCheatslips(m_email, m_password);
    if (token.empty()) {
        App::Notify("Login failed. Check your credentials.");
    } else {
        SaveCheatslipsToken(token);
        App::Notify("Logged in successfully!");
    }
    SetPop();
}

} // namespace sphaira::ui::menu::hats
