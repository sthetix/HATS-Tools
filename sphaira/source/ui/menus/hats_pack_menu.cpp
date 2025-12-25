#include "ui/menus/hats_pack_menu.hpp"

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
#include "threaded_file_transfer.hpp"
#include "utils/utils.hpp"

#include <yyjson.h>
#include <dirent.h>
#include <cstring>

namespace sphaira::ui::menu::hats {

namespace {

constexpr const char* HATS_API_URL = "https://api.github.com/repos/sthetix/HATS/releases";
constexpr const char* CACHE_PATH = "/switch/hats-tools/cache/hats";
constexpr const char* RELEASES_CACHE = "/switch/hats-tools/cache/hats/releases.json";
constexpr const char* DOWNLOAD_TEMP = "/switch/hats-tools/cache/hats/download.zip";

// HATS paths are now configurable via config.ini under [hats] section
// Defaults: installer_payload=/bootloader/payloads/hats-installer.bin
//           staging_path=/hats-staging

void from_json(yyjson_val* json, ReleaseEntry& e) {
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

void from_json(const fs::FsPath& path, std::vector<ReleaseEntry>& entries) {
    JSON_INIT_VEC_FILE(path, nullptr, nullptr);
    if (yyjson_is_arr(json)) {
        JSON_ARR_ITR(entries);
    }
}

auto DownloadAndExtract(ProgressBox* pbox, const ReleaseEntry& release) -> Result {
    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());

    // Get HATS paths from config
    auto app = App::GetApp();
    const fs::FsPath staging_path = app->m_hats_staging_path.Get().c_str();

    // Ensure cache directory exists
    fs.CreateDirectoryRecursively(CACHE_PATH);

    // Clean up staging directory if it exists, then recreate it
    log_write("hats: cleaning staging directory: %s\n", static_cast<const char*>(staging_path));
    if (fs.DirExists(staging_path)) {
        Result rc = fs.DeleteDirectoryRecursively(staging_path);
        if (R_FAILED(rc)) {
            log_write("hats: warning - failed to delete staging directory: 0x%X\n", rc);
            // Continue anyway, we'll try to extract over it
        } else {
            log_write("hats: successfully deleted staging directory\n");
        }
    }
    fs.CreateDirectoryRecursively(staging_path);

    // Clean up any existing temp file
    fs.DeleteFile(DOWNLOAD_TEMP);

    // Download the ZIP
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer("Downloading " + release.asset_name);
        log_write("hats: downloading %s\n", release.download_url.c_str());

        const auto result = curl::Api().ToFile(
            curl::Url{release.download_url},
            curl::Path{DOWNLOAD_TEMP},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );

        R_UNLESS(result.success, 0x1);
    }

    // Extract to staging directory
    if (!pbox->ShouldExit()) {
        pbox->NewTransfer("Preparing installation...");
        log_write("hats: extracting to staging directory\n");

        bool download_exists = fs.FileExists(DOWNLOAD_TEMP);
        log_write("hats: download file exists: %s\n", download_exists ? "yes" : "no");

        if (download_exists) {
            Result rc = thread::TransferUnzipAll(pbox, DOWNLOAD_TEMP, &fs, staging_path,
                [&](const fs::FsPath& name, fs::FsPath& path) -> bool {
                    log_write("hats: extracting file: %s -> %s\n", static_cast<const char*>(name), static_cast<const char*>(path));
                    return true;  // Extract all files
                });

            log_write("hats: extraction completed with result: 0x%X\n", rc);

            if (R_FAILED(rc)) {
                log_write("hats: extraction failed with error: 0x%X\n", rc);
                return rc;
            }
        } else {
            log_write("hats: ERROR - download file does not exist!\n");
            return 0x2;
        }
    }

    // Commit file system changes
    if (!pbox->ShouldExit()) {
        log_write("hats: committing file system changes\n");
        Result commit_result = fs.Commit();
        log_write("hats: commit result: 0x%X\n", commit_result);
        R_TRY(commit_result);
    }

    // Verify staging files exist
    log_write("hats: verifying staging files...\n");
    // Build paths dynamically based on staging_path
    fs::FsPath staging_dirs[] = {
        staging_path + "/atmosphere",
        staging_path + "/bootloader",
        staging_path + "/switch"
    };
    for (const auto& dir : staging_dirs) {
        bool exists = fs.DirExists(dir);
        log_write("hats: %s exists: %s\n", static_cast<const char*>(dir), exists ? "yes" : "no");
    }

    // Clean up temp file
    fs.DeleteFile(DOWNLOAD_TEMP);

    log_write("hats: staging complete\n");
    return 0;
}

} // namespace

PackMenu::PackMenu() : MenuBase{"HATS Pack Releases", MenuFlag_None} {
    fs::FsNativeSd().CreateDirectoryRecursively(CACHE_PATH);

    this->SetActions(
        std::make_pair(Button::A, Action{"Install"_i18n, [this](){
            if (!m_releases.empty() && !m_loading) {
                DownloadAndInstall();
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

PackMenu::~PackMenu() {
}

void PackMenu::Update(Controller* controller, TouchInfo* touch) {
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

void PackMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

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

        // Format date (published_at is like "2024-12-17T10:00:00Z")
        std::string date = release.published_at.substr(0, 10);

        // Display name
        std::string display_name = release.name.empty() ? release.tag_name : release.name;
        if (release.prerelease) {
            display_name += " (Pre-Release)";
        }

        gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f, 20.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
            theme->GetColour(text_id),
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

void PackMenu::OnFocusGained() {
    MenuBase::OnFocusGained();

    if (!m_loaded && !m_loading) {
        FetchReleases();
    }
}

void PackMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
    UpdateSubheading();
}

void PackMenu::FetchReleases() {
    m_loading = true;
    m_error_message.clear();
    m_releases.clear();

    curl::Api().ToFileAsync(
        curl::Url{HATS_API_URL},
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
                log_write("hats: failed to fetch releases\n");
                return false;
            }

            from_json(result.path, m_releases);

            if (m_releases.empty()) {
                m_error_message = "No releases found.";
            } else {
                log_write("hats: loaded %zu releases\n", m_releases.size());
                SetIndex(0);
            }

            return true;
        }}
    );
}

void PackMenu::DownloadAndInstall() {
    if (m_releases.empty() || m_index >= (s64)m_releases.size()) {
        return;
    }

    const auto& release = m_releases[m_index];
    std::string display_name = release.name.empty() ? release.tag_name : release.name;

    // Get staging path from config for display message
    auto app = App::GetApp();
    const fs::FsPath staging_path = app->m_hats_staging_path.Get().c_str();

    App::Push<OptionBox>(
        "Download " + display_name + "?\n\n"
        "Files will be extracted to " + std::string(staging_path) + ".",
        "Cancel"_i18n, "Download"_i18n, 1, [this, release, display_name](auto op_index) {
            if (!op_index || *op_index != 1) {
                return;
            }

            App::Push<ProgressBox>(0, "Installing"_i18n, display_name,
                [release](auto pbox) -> Result {
                    return DownloadAndExtract(pbox, release);
                },
                [display_name](Result rc) {
                    if (R_SUCCEEDED(rc)) {
                        fs::FsNativeSd fs;
                        // Get installer payload path from config
                        auto app = App::GetApp();
                        const fs::FsPath installer_payload = app->m_hats_installer_payload.Get().c_str();

                        log_write("hats: checking for HATS installer at: %s\n", static_cast<const char*>(installer_payload));

                        bool installer_exists = fs.FileExists(installer_payload);
                        log_write("hats: installer file exists: %s\n", installer_exists ? "yes" : "no");

                        if (installer_exists) {
                            // Show confirmation dialog before launching payload
                            App::Push<OptionBox>(
                                display_name + " downloaded",
                                "Back"_i18n, "Launch"_i18n, 1, [installer_payload](auto op_index) {
                                    if (!op_index || *op_index != 1) {
                                        log_write("hats: user chose not to launch installer\n");
                                        return;
                                    }

                                    log_write("hats: launching HATS installer: %s\n", static_cast<const char*>(installer_payload));
                                    bool reboot_success = utils::rebootToPayload(installer_payload);
                                    log_write("hats: rebootToPayload result: %s\n", reboot_success ? "success" : "failed");
                                    if (!reboot_success) {
                                        App::Push<OptionBox>("Failed to launch HATS installer!\n\n" + std::string(installer_payload), "OK"_i18n, ""_i18n, 0, [](auto op_index) {});
                                    }
                                }
                            );
                        } else {
                            log_write("hats: HATS installer not found at: %s\n", static_cast<const char*>(installer_payload));
                            App::Push<OptionBox>("HATS-installer payload not found", "OK"_i18n, ""_i18n, 0, [](auto op_index) {});
                        }
                    } else {
                        App::Push<ErrorBox>(rc, "Failed to download " + display_name);
                    }
                }
            );
        }
    );
}

void PackMenu::UpdateSubheading() {
    const auto index = m_releases.empty() ? 0 : m_index + 1;
    this->SetSubHeading(std::to_string(index) + " / " + std::to_string(m_releases.size()));
}

} // namespace sphaira::ui::menu::hats
