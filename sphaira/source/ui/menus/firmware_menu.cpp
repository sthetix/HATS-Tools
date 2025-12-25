#include "ui/menus/firmware_menu.hpp"

#include "ui/nvg_util.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/error_box.hpp"

#include "app.hpp"
#include "log.hpp"
#include "download.hpp"
#include "fs.hpp"
#include "i18n.hpp"
#include "yyjson_helper.hpp"
#include "hats_version.hpp"
#include "threaded_file_transfer.hpp"

#include <yyjson.h>
#include <cstring>
#include <sstream>

namespace sphaira::ui::menu::hats {

namespace {

constexpr const char* NXFW_API_URL = "https://api.github.com/repos/sthetix/NXFW/releases";
constexpr const char* CACHE_PATH = "/switch/hats-tools/cache/hats";
constexpr const char* RELEASES_CACHE = "/switch/hats-tools/cache/hats/firmware_releases.json";
constexpr const char* DOWNLOAD_TEMP = "/switch/hats-tools/cache/hats/firmware.zip";
constexpr const char* FIRMWARE_DEST = "/firmware";

void from_json(yyjson_val* json, FirmwareEntry& e) {
    JSON_OBJ_ITR(
        JSON_SET_STR(tag_name);
        JSON_SET_STR(name);
        JSON_SET_STR(published_at);
        JSON_SET_BOOL(prerelease);

        case cexprHash("assets"): {
            if (yyjson_is_arr(val)) {
                size_t idx, max;
                yyjson_val* hit;
                yyjson_arr_foreach(val, idx, max, hit) {
                    if (!yyjson_is_obj(hit)) continue;

                    auto name_val = yyjson_obj_get(hit, "name");
                    auto url_val = yyjson_obj_get(hit, "browser_download_url");
                    auto size_val = yyjson_obj_get(hit, "size");

                    if (name_val && url_val) {
                        const char* name_str = yyjson_get_str(name_val);
                        if (name_str && strstr(name_str, ".zip")) {
                            e.asset_name = name_str;
                            e.download_url = yyjson_get_str(url_val);
                            if (size_val) {
                                e.size = yyjson_get_uint(size_val);
                            }
                            break;
                        }
                    }
                }
            }
        } break;
    );
}

void from_json(const fs::FsPath& path, std::vector<FirmwareEntry>& entries) {
    JSON_INIT_VEC_FILE(path, nullptr, nullptr);
    if (yyjson_is_arr(json)) {
        JSON_ARR_ITR(entries);
    }
}

// Parse version string into components for comparison
std::vector<int> parseVersion(const std::string& version) {
    std::vector<int> parts;
    std::stringstream ss(version);
    std::string segment;

    while (std::getline(ss, segment, '.')) {
        if (segment.empty()) continue;
        // Extract numeric part only using strtol (no exceptions)
        char* end = nullptr;
        long part = std::strtol(segment.c_str(), &end, 10);
        if (end != segment.c_str()) {
            parts.push_back(static_cast<int>(part));
        } else {
            break;
        }
    }

    return parts;
}

// Compare versions: returns true if target < current (downgrade)
bool isVersionLower(const std::string& target, const std::string& current) {
    auto target_parts = parseVersion(target);
    auto current_parts = parseVersion(current);

    size_t max_len = std::max(target_parts.size(), current_parts.size());

    for (size_t i = 0; i < max_len; i++) {
        int t = (i < target_parts.size()) ? target_parts[i] : 0;
        int c = (i < current_parts.size()) ? current_parts[i] : 0;

        if (t < c) return true;
        if (t > c) return false;
    }

    return false;
}

auto DownloadAndExtract(ProgressBox* pbox, const FirmwareEntry& release) -> Result {
    fs::FsNativeSd fs;
    Result fs_result = fs.GetFsOpenResult();
    log_write("firmware: FsNativeSd initialization result: 0x%X\n", fs_result);
    R_TRY(fs_result);

    // Clean up firmware directory if it exists, then recreate it
    log_write("firmware: cleaning firmware directory: %s\n", FIRMWARE_DEST);
    if (fs.DirExists(FIRMWARE_DEST)) {
        Result rc = fs.DeleteDirectoryRecursively(FIRMWARE_DEST);
        if (R_FAILED(rc)) {
            log_write("firmware: warning - failed to delete firmware directory: 0x%X\n", rc);
            // Continue anyway, we'll try to extract over it
        } else {
            log_write("firmware: successfully deleted firmware directory\n");
        }
    }
    fs.CreateDirectoryRecursively(FIRMWARE_DEST);

    // Ensure cache directory exists
    log_write("firmware: creating cache directory %s\n", CACHE_PATH);
    bool cache_created = fs.CreateDirectoryRecursively(CACHE_PATH);
    log_write("firmware: cache directory creation: %s\n", cache_created ? "success" : "failed");

    // Clean up any existing temp file
    if (fs.FileExists(DOWNLOAD_TEMP)) {
        log_write("firmware: deleting existing temp file %s\n", DOWNLOAD_TEMP);
        Result del_result = fs.DeleteFile(DOWNLOAD_TEMP);
        log_write("firmware: temp file deletion result: 0x%X\n", del_result);
    }

    // Download the ZIP
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer("Downloading " + release.asset_name);
        log_write("firmware: starting download of %s\n", release.download_url.c_str());
        log_write("firmware: release asset: %s (size: %zu bytes)\n",
                  release.asset_name.c_str(), release.size);

        const auto result = curl::Api().ToFile(
            curl::Url{release.download_url},
            curl::Path{DOWNLOAD_TEMP},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );

        if (!result.success) {
            log_write("firmware: download failed!\n");
            R_UNLESS(result.success, 0x1);
        }

        // Verify downloaded file exists
        bool download_exists = fs.FileExists(DOWNLOAD_TEMP);
        log_write("firmware: download completed, file exists: %s\n", download_exists ? "yes" : "no");
    }

    // Extract to /firmware
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer("Extracting to /firmware...");
        log_write("firmware: starting extraction to %s\n", FIRMWARE_DEST);

        Result extract_result = thread::TransferUnzipAll(pbox, DOWNLOAD_TEMP, &fs, FIRMWARE_DEST);
        log_write("firmware: extraction result: 0x%X\n", extract_result);

        if (R_FAILED(extract_result)) {
            log_write("firmware: extraction failed!\n");
            return extract_result;
        }

        // Commit file system changes
        log_write("firmware: committing file system changes\n");
        Result commit_result = fs.Commit();
        log_write("firmware: commit result: 0x%X\n", commit_result);
        R_TRY(commit_result);
    }

    // Clean up temp file
    log_write("firmware: cleaning up temp file %s\n", DOWNLOAD_TEMP);
    Result cleanup_result = fs.DeleteFile(DOWNLOAD_TEMP);
    log_write("firmware: temp file cleanup result: 0x%X\n", cleanup_result);

    log_write("firmware: extraction complete\n");
    return 0;
}

} // namespace

FirmwareMenu::FirmwareMenu() : MenuBase{"Firmware Releases", MenuFlag_None} {
    fs::FsNativeSd().CreateDirectoryRecursively(CACHE_PATH);

    m_current_firmware = sphaira::hats::getSystemFirmware();

    this->SetActions(
        std::make_pair(Button::A, Action{"Download"_i18n, [this](){
            if (!m_releases.empty() && !m_loading) {
                DownloadFirmware();
            }
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){
            SetPop();
        }}),
        std::make_pair(Button::X, Action{"Refresh"_i18n, [this](){
            m_loaded = false;
            FetchReleases();
        }})
    );

    const Vec4 v{75, GetY() + 1.f + 42.f, 1220.f - 150.f, 60.f};
    m_list = std::make_unique<List>(1, 8, m_pos, v);
    m_list->SetLayout(List::Layout::GRID);
}

FirmwareMenu::~FirmwareMenu() {
}

void FirmwareMenu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (!m_releases.empty()) {
        m_list->OnUpdate(controller, touch, m_index, m_releases.size(), [this](bool touch, auto i) {
            if (touch && m_index == i) {
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                SetIndex(i);
            }
        });
    }
}

void FirmwareMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    // Draw current firmware info
    gfx::drawTextArgs(vg, 80.f, GetY() + 10.f, 18.f,
        NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "Current Firmware: %s", m_current_firmware.c_str());

    if (m_loading) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "Loading releases...");
        return;
    }

    if (!m_error_message.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_ERROR),
            "%s", m_error_message.c_str());
        return;
    }

    if (m_releases.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 24.f,
            NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE,
            theme->GetColour(ThemeEntryID_TEXT_INFO),
            "No releases found");
        return;
    }

    constexpr float text_xoffset{15.f};

    m_list->Draw(vg, theme, m_releases.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& release = m_releases[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else {
            if (i != m_releases.size() - 1) {
                gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
            }
        }

        // Format date
        std::string date = release.published_at.substr(0, 10);

        // Display name - typically the firmware version
        std::string display_name = release.name.empty() ? release.tag_name : release.name;
        if (release.prerelease) {
            display_name += " (Pre-Release)";
        }

        // Check if this would be a downgrade
        bool is_downgrade = IsDowngrade(release.tag_name);
        if (is_downgrade) {
            display_name += " [DOWNGRADE]";
        }

        auto name_color = is_downgrade ? ThemeEntryID_ERROR : text_id;

        gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f, 20.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            theme->GetColour(name_color),
            "[%s] %s", date.c_str(), display_name.c_str());

        // Size on the right
        if (release.size > 0) {
            float size_mb = release.size / 1024.0f / 1024.0f;
            gfx::drawTextArgs(vg, x + w - text_xoffset, y + h / 2.f, 16.f,
                NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE,
                theme->GetColour(ThemeEntryID_TEXT_INFO),
                "%.1f MB", size_mb);
        }
    });
}

void FirmwareMenu::OnFocusGained() {
    MenuBase::OnFocusGained();

    if (!m_loaded && !m_loading) {
        FetchReleases();
    }
}

void FirmwareMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
    UpdateSubheading();
}

void FirmwareMenu::FetchReleases() {
    m_loading = true;
    m_error_message.clear();
    m_releases.clear();

    curl::Api().ToFileAsync(
        curl::Url{NXFW_API_URL},
        curl::Path{RELEASES_CACHE},
        curl::Flags{curl::Flag_Cache},
        curl::StopToken{this->GetToken()},
        curl::Header{
            {"Accept", "application/vnd.github+json"},
        },
        curl::OnComplete{[this](auto& result) {
            m_loading = false;
            m_loaded = true;

            if (!result.success) {
                m_error_message = "Failed to fetch releases. Check your internet connection.";
                log_write("firmware: failed to fetch releases\n");
                return false;
            }

            from_json(result.path, m_releases);

            if (m_releases.empty()) {
                m_error_message = "No releases found.";
            } else {
                log_write("firmware: loaded %zu releases\n", m_releases.size());
                SetIndex(0);
            }

            return true;
        }}
    );
}

void FirmwareMenu::DownloadFirmware() {
    if (m_releases.empty() || m_index >= (s64)m_releases.size()) {
        return;
    }

    const auto& release = m_releases[m_index];
    std::string display_name = release.name.empty() ? release.tag_name : release.name;

    bool is_downgrade = IsDowngrade(release.tag_name);

    std::string message = "Download firmware " + display_name + "?\n\n";
    message += "Firmware will be extracted to /firmware.";

    if (is_downgrade) {
        message = "WARNING: This is a DOWNGRADE!\n\n";
        message += "Current: " + m_current_firmware + "\n";
        message += "Target: " + display_name + "\n\n";
        message += "Downgrading firmware can cause issues.\nProceed with caution!";
    }

    App::Push<OptionBox>(
        message,
        "Cancel"_i18n, is_downgrade ? "Downgrade" : "Download", 1,
        [this, release, display_name](auto op_index) {
            if (!op_index || *op_index != 1) {
                return;
            }

            App::Push<ProgressBox>(0, "Downloading"_i18n, display_name,
                [release](auto pbox) -> Result {
                    return DownloadAndExtract(pbox, release);
                },
                [display_name](Result rc) {
                    if (R_SUCCEEDED(rc)) {
                        App::Notify("Downloaded " + display_name);
                        App::Push<OptionBox>(
                            "Firmware extracted to /firmware.\n\nUse Daybreak to install it.",
                            "OK"_i18n
                        );
                    } else {
                        App::Push<ErrorBox>(rc, "Failed to download " + display_name);
                    }
                }
            );
        }
    );
}

void FirmwareMenu::UpdateSubheading() {
    const auto index = m_releases.empty() ? 0 : m_index + 1;
    this->SetSubHeading(std::to_string(index) + " / " + std::to_string(m_releases.size()));
}

bool FirmwareMenu::IsDowngrade(const std::string& target_version) {
    return isVersionLower(target_version, m_current_firmware);
}

} // namespace sphaira::ui::menu::hats
