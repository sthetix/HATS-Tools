#include "ui/menus/hats_main_menu.hpp"
#include "ui/menus/hats_pack_menu.hpp"
#include "ui/menus/hats_game_hub_menu.hpp"
#include "ui/menus/hats_network_menu.hpp"
#include "ui/menus/hats_settings_menu.hpp"
#include "ui/menus/firmware_menu.hpp"
#include "ui/menus/uninstaller_menu.hpp"
#include "ui/menus/cheats_menu.hpp"
#include "ui/menus/filebrowser.hpp"
#include "ui/menus/appstore.hpp"

#include "ui/nvg_util.hpp"
#include "ui/option_box.hpp"
#include "ui/progress_box.hpp"
#include "ui/error_box.hpp"
#include "ui/popup_list.hpp"

#include "app.hpp"
#include "app_version.hpp"
#include "defines.hpp"
#include "download.hpp"
#include "log.hpp"
#include "hats_version.hpp"
#include "i18n.hpp"
#include "threaded_file_transfer.hpp"
#include "utils/utils.hpp"
#include "yati/nx/ns.hpp"

#include "stb_image.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <yyjson.h>

namespace sphaira::ui::menu::hats {
namespace {

constexpr s64 WIPE_COUNTDOWN_MIN_SECONDS = 5;
constexpr s64 WIPE_COUNTDOWN_MAX_SECONDS = 60;
constexpr u64 TOOLS_UPDATE_RETRY_DELAY_NS[] = {
    10'000'000'000ULL,
    30'000'000'000ULL,
    60'000'000'000ULL,
};
constexpr const char* HATS_TOOLS_RELEASE_URL = "https://api.github.com/repos/sthetix/HATS-Tools/releases/latest";
constexpr const char* HATS_TOOLS_RELEASE_CACHE = "/switch/hats-tools/cache/hats-tools-latest.json";
constexpr const char* HATS_TOOLS_UPDATE_ZIP = "/switch/hats-tools/cache/hats-tools-update.zip";

struct ToolsUpdateInfo {
    std::string version;
    std::string url;
};

auto GetElapsedSeconds(u64 start_tick) -> s64 {
    if (!start_tick) {
        return 0;
    }

    const auto elapsed_ns = armTicksToNs(armGetSystemTick() - start_tick);
    return static_cast<s64>(elapsed_ns / 1'000'000'000ULL);
}

auto IsRunningEmummc(bool* out) -> Result {
    *out = false;

    Result rc = splInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }

    u64 emummc{};
    rc = splGetConfig((SplConfigItem)65007, &emummc);
    splExit();

    if (R_SUCCEEDED(rc)) {
        *out = emummc != 0;
    }

    return rc;
}

constexpr Result CLEAN_EMUMMC_BLOCKED = MAKERESULT(Module_Libnx, LibnxError_BadInput);

struct CleanEmummcPlan {
    std::string nintendo_path;
    std::vector<u64> applications;
    std::vector<FsSaveDataInfo> saves;
    std::vector<std::string> identity;
    PopupList::Items details;
    size_t updates{};
    size_t dlcs{};
    size_t account_saves{};
    size_t device_saves{};
};

auto CleanId(u64 id) -> std::string {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(id));
    return text;
}

bool IsCleanUserApplication(u64 id) {
    return (id >= 0x0100000000010000ULL && id < 0x0200000000000000ULL) ||
        (id >> 56) == 0x05;
}

auto VerifyCleanEmummc(std::string& nintendo_path) -> Result {
    bool emummc{};
    R_TRY(IsRunningEmummc(&emummc));
    R_UNLESS(emummc, CLEAN_EMUMMC_BLOCKED);

    // Query the running secure monitor, not an INI which may have changed since boot.
    alignas(0x1000) AmsEmummcPaths paths{};
    SecmonArgs args{};
    args.X[0] = 0xF0000404;
    args.X[2] = reinterpret_cast<u64>(&paths);
    svcCallSecureMonitor(&args);
    R_UNLESS(args.X[0] == 0, CLEAN_EMUMMC_BLOCKED);
    R_UNLESS(static_cast<u32>(args.X[1]) == 0x30534645 &&
        ((args.X[1] >> 32) == 1 || (args.X[1] >> 32) == 2), CLEAN_EMUMMC_BLOCKED);
    R_UNLESS(std::memchr(paths.nintendo, 0, sizeof(paths.nintendo)), CLEAN_EMUMMC_BLOCKED);

    nintendo_path = paths.nintendo;
    if (nintendo_path.empty()) {
        // Match Atmosphere's default when no explicit Nintendo path was configured.
        char fallback[48];
        std::snprintf(fallback, sizeof(fallback), "emummc/Nintendo_%04x", static_cast<u32>(args.X[2]));
        nintendo_path = fallback;
    }
    if (nintendo_path.starts_with('/')) {
        nintendo_path.erase(0, 1);
    }
    std::transform(nintendo_path.begin(), nintendo_path.end(), nintendo_path.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // Restrict cleaning to a redirected Nintendo directory inside emuMMC.
    R_UNLESS(nintendo_path.starts_with("emummc/") && nintendo_path.size() > 7 &&
        !nintendo_path.ends_with('/') &&
        nintendo_path.find("//") == std::string::npos &&
        nintendo_path.find('\\') == std::string::npos &&
        nintendo_path.find(':') == std::string::npos, CLEAN_EMUMMC_BLOCKED);
    for (size_t start = 0; start < nintendo_path.size();) {
        const auto end = nintendo_path.find('/', start);
        const auto part = nintendo_path.substr(start, end - start);
        R_UNLESS(part != "." && part != "..", CLEAN_EMUMMC_BLOCKED);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return 0;
}

auto ScanCleanEmummc(ProgressBox* pbox, CleanEmummcPlan& plan) -> Result {
    plan = {};
    R_TRY(VerifyCleanEmummc(plan.nintendo_path));
    R_TRY(nsInitialize());
    ON_SCOPE_EXIT(nsExit());
    pbox->NewTransfer("Scanning installed games...");
    NsApplicationRecord records[64];
    for (s32 offset = 0;;) {
        R_TRY(pbox->ShouldExitResult());
        s32 count{};
        R_TRY(nsListApplicationRecord(records, 64, offset, &count));
        R_UNLESS(count >= 0 && count <= 64, CLEAN_EMUMMC_BLOCKED);
        if (!count) {
            break;
        }
        for (s32 i = 0; i < count; i++) {
            const auto id = records[i].application_id;
            if (!IsCleanUserApplication(id) ||
                std::find(plan.applications.begin(), plan.applications.end(), id) != plan.applications.end()) {
                continue;
            }
            plan.applications.push_back(id);
            bool installed{};
            R_TRY(nsIsAnyApplicationEntityInstalled(id, &installed));
            plan.identity.push_back("app:" + CleanId(id) + ":" + std::to_string(installed));
            s32 meta_count{};
            const auto meta_rc = nsCountApplicationContentMeta(id, &meta_count);
            if (meta_rc == 0x410 && !installed) { // NS not found, also handled by the forwarder installer.
                meta_count = 0;
            } else {
                R_TRY(meta_rc);
            }
            R_UNLESS(meta_count >= 0, CLEAN_EMUMMC_BLOCKED);
            std::vector<NsApplicationContentMetaStatus> metas(meta_count);
            s32 read{};
            if (meta_count) {
                R_TRY(nsListApplicationContentMetaStatus(id, 0, metas.data(), meta_count, &read));
                R_UNLESS(read == meta_count, CLEAN_EMUMMC_BLOCKED);
            }
            size_t updates{}, dlcs{};
            for (const auto& meta : metas) {
                R_UNLESS(meta.meta_type == NcmContentMetaType_Application ||
                    meta.meta_type == NcmContentMetaType_Patch ||
                    meta.meta_type == NcmContentMetaType_AddOnContent ||
                    meta.meta_type == NcmContentMetaType_DataPatch ||
                    meta.meta_type == NcmContentMetaType_Delta, CLEAN_EMUMMC_BLOCKED);
                R_UNLESS(meta.storageID == NcmStorageId_BuiltInUser ||
                    meta.storageID == NcmStorageId_SdCard || meta.storageID == NcmStorageId_GameCard ||
                    meta.storageID == NcmStorageId_None, CLEAN_EMUMMC_BLOCKED);
                updates += meta.meta_type == NcmContentMetaType_Patch ||
                    meta.meta_type == NcmContentMetaType_DataPatch;
                dlcs += meta.meta_type == NcmContentMetaType_AddOnContent;
                plan.identity.push_back("meta:" + CleanId(id) + ":" + CleanId(meta.application_id) +
                    ":" + std::to_string(meta.meta_type) + ":" + std::to_string(meta.storageID) +
                    ":" + std::to_string(meta.version));
            }
            plan.updates += updates;
            plan.dlcs += dlcs;
            plan.details.push_back("Game " + CleanId(id) + " | updates " + std::to_string(updates) +
                " | DLC " + std::to_string(dlcs) + (installed ? "" : " | no installed content"));
        }
        offset += count;
    }

    pbox->NewTransfer("Scanning saves for all users...");
    FsSaveDataInfoReader reader{};
    R_TRY(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User));
    ON_SCOPE_EXIT(fsSaveDataInfoReaderClose(&reader));
    FsSaveDataInfo saves[64];
    for (;;) {
        R_TRY(pbox->ShouldExitResult());
        s64 count{};
        R_TRY(fsSaveDataInfoReaderRead(&reader, saves, 64, &count));
        R_UNLESS(count >= 0 && count <= 64, CLEAN_EMUMMC_BLOCKED);
        if (!count) {
            break;
        }
        for (s64 i = 0; i < count; i++) {
            const auto& save = saves[i];
            if (save.save_data_type != FsSaveDataType_Account && save.save_data_type != FsSaveDataType_Device) {
                continue;
            }
            if (!IsCleanUserApplication(save.application_id) || save.system_save_data_id) {
                continue;
            }
            R_UNLESS(save.save_data_space_id == FsSaveDataSpaceId_User && save.save_data_id,
                CLEAN_EMUMMC_BLOCKED);
            plan.saves.push_back(save);
            const bool account = save.save_data_type == FsSaveDataType_Account;
            plan.account_saves += account;
            plan.device_saves += !account;
            const auto identity = CleanId(save.save_data_id) + ":" + CleanId(save.application_id) +
                ":" + CleanId(save.uid.uid[0]) + CleanId(save.uid.uid[1]) +
                ":" + std::to_string(save.save_data_type) + ":" + std::to_string(save.save_data_space_id) +
                ":" + std::to_string(save.save_data_rank) + ":" + std::to_string(save.save_data_index);
            plan.identity.push_back("save:" + identity);
            plan.details.push_back(std::string(account ? "Account save " : "Device save ") +
                CleanId(save.save_data_id) + " | game " + CleanId(save.application_id));
            if (account) {
                plan.details.push_back("  User " + CleanId(save.uid.uid[0]) + CleanId(save.uid.uid[1]));
            }
        }
    }
    std::sort(plan.identity.begin(), plan.identity.end());
    std::string path;
    R_TRY(VerifyCleanEmummc(path));
    R_UNLESS(path == plan.nintendo_path, CLEAN_EMUMMC_BLOCKED);
    return 0;
}

void RunCleanEmummc(const std::shared_ptr<CleanEmummcPlan>& plan) {
    auto report = std::make_shared<PopupList::Items>();
    App::Push<ProgressBox>(0, "Clean emuMMC", "Verifying targets...",
        [plan, report](auto pbox) -> Result {
            CleanEmummcPlan current;
            R_TRY(ScanCleanEmummc(pbox, current));
            R_UNLESS(current.nintendo_path == plan->nintendo_path && current.identity == plan->identity,
                CLEAN_EMUMMC_BLOCKED);
            R_TRY(ns::Initialize());
            ON_SCOPE_EXIT(ns::Exit());
            size_t succeeded{}, failed{}, completed{};
            const auto total = plan->saves.size() + plan->applications.size();
            auto record = [&](const std::string& target, Result rc) {
                if (R_SUCCEEDED(rc)) {
                    succeeded++;
                } else {
                    failed++;
                }
                const auto line = target + (R_SUCCEEDED(rc) ? " OK" : " FAILED 0x" + CleanId(rc));
                report->push_back(line);
                log_write("[Clean emuMMC] %s\n", line.c_str());
                pbox->UpdateTransfer(++completed, total);
            };
            auto verify = [&]() -> Result {
                std::string path;
                R_TRY(VerifyCleanEmummc(path));
                R_UNLESS(path == plan->nintendo_path, CLEAN_EMUMMC_BLOCKED);
                return 0;
            };
            auto erase = [&]() -> Result {
                // Delete saves first so application deletion cannot make planned saves disappear first.
                for (const auto& save : plan->saves) {
                    R_TRY(verify());
                    pbox->NewTransfer("Deleting save " + CleanId(save.save_data_id));
                    FsSaveDataExtraData extra{};
                    Result rc = fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof(extra),
                        FsSaveDataSpaceId_User, save.save_data_id);
                    if (R_SUCCEEDED(rc)) {
                        const auto& attr = extra.attr;
                        if (attr.application_id != save.application_id || attr.system_save_data_id ||
                            attr.save_data_type != save.save_data_type ||
                            attr.uid.uid[0] != save.uid.uid[0] || attr.uid.uid[1] != save.uid.uid[1] ||
                            attr.save_data_rank != save.save_data_rank || attr.save_data_index != save.save_data_index) {
                            rc = CLEAN_EMUMMC_BLOCKED;
                        } else {
                            rc = fsDeleteSaveDataFileSystemBySaveDataSpaceId(FsSaveDataSpaceId_User, save.save_data_id);
                        }
                    }
                    record("Save " + CleanId(save.save_data_id), rc);
                }
                for (const auto id : plan->applications) {
                    R_TRY(verify());
                    pbox->NewTransfer("Deleting game " + CleanId(id));
                    auto rc = nsDeleteApplicationCompletely(id);
                    if (R_FAILED(rc)) {
                        report->push_back("nsDeleteApplicationCompletely " + CleanId(id) + " FAILED 0x" + CleanId(rc));
                        bool installed{};
                        const auto check_rc = nsIsAnyApplicationEntityInstalled(id, &installed);
                        if (R_SUCCEEDED(check_rc) && !installed) {
                            // A cloned/archived record can remain even when no content is available.
                            R_TRY(verify());
                            rc = ns::DeleteApplicationRecord(id);
                            report->push_back("Record-only removal " + CleanId(id) +
                                (R_SUCCEEDED(rc) ? " OK" : " FAILED 0x" + CleanId(rc)));
                        } else if (R_FAILED(check_rc)) {
                            report->push_back("Content check " + CleanId(id) + " FAILED 0x" + CleanId(check_rc));
                        }
                    }
                    record("Game " + CleanId(id), rc);
                }
                return 0;
            };
            const auto erase_rc = erase();
            report->insert(report->begin(), "Targets: " + std::to_string(succeeded) +
                " succeeded, " + std::to_string(failed) + " failed");
            pbox->NewTransfer("Scanning remaining targets...");
            CleanEmummcPlan remaining;
            const auto scan_rc = ScanCleanEmummc(pbox, remaining);
            if (R_FAILED(scan_rc)) {
                report->insert(report->begin(), "Rescan FAILED 0x" + CleanId(scan_rc) + "; remaining data unknown");
            } else {
                report->insert(report->begin(), "Remaining: " + std::to_string(remaining.applications.size()) +
                    " game records, " + std::to_string(remaining.saves.size()) + " saves");
            }
            if (R_FAILED(erase_rc)) {
                report->insert(report->begin(), "Cleaning stopped: environment verification failed 0x" + CleanId(erase_rc));
            }
            report->insert(report->begin(), "Not processed: " + std::to_string(total - completed));
            return 0;
        },
        [report](Result rc) {
            if (R_FAILED(rc)) {
                App::PushErrorBox(rc, "Clean emuMMC blocked: verification or target scan failed. Scan again.");
                return;
            }
            App::Push<PopupList>("Clean emuMMC results", *report, PopupList::Callback{});
        }, false);
}

void StartCleanEmummcFlow() {
    if (!hosversionAtLeast(2, 0, 0)) {
        App::Push<OptionBox>("Clean emuMMC requires HOS 2.0.0 or newer.", "OK"_i18n);
        return;
    }
    auto plan = std::make_shared<CleanEmummcPlan>();
    App::Push<ProgressBox>(0, "Scan emuMMC", "", [plan](auto pbox) -> Result {
        return ScanCleanEmummc(pbox, *plan);
    }, [plan](Result rc) {
        if (rc == Result_TransferCancelled) {
            return;
        }
        if (R_FAILED(rc)) {
            App::PushErrorBox(rc, "Clean emuMMC blocked: scan failed or Nintendo path is not isolated inside emuMMC.");
            return;
        }
        if (plan->applications.empty() && plan->saves.empty()) {
            App::Push<OptionBox>("No user games or Account/Device saves found.", "OK"_i18n);
            return;
        }
        auto details = plan->details;
        details.insert(details.begin(), {
            "Continue to confirmation",
            "Nintendo: /" + plan->nintendo_path,
            "Games: " + std::to_string(plan->applications.size()) + " | Updates: " +
                std::to_string(plan->updates) + " | DLC: " + std::to_string(plan->dlcs),
            "Account saves: " + std::to_string(plan->account_saves) +
                " | Device saves: " + std::to_string(plan->device_saves),
            "Accounts, settings and system data are kept",
        });
        App::Push<PopupList>("Clean emuMMC - review targets (B cancels)", details,
            [plan](auto index) {
                if (!index || *index != 0) {
                    return;
                }
                App::Push<OptionBox>("Delete " + std::to_string(plan->applications.size()) +
                    " games (including updates/DLC) and " + std::to_string(plan->saves.size()) +
                    " saves from emuMMC?\nAccounts and settings are kept.\nThis cannot be undone.",
                    "Cancel"_i18n, "Clean emuMMC", 0, [plan](auto choice) {
                        if (choice && *choice == 1) {
                            RunCleanEmummc(plan);
                        }
                    });
            });
    });
}

auto InstallToolsUpdateZip(ProgressBox* pbox, const std::string& url, const std::string& version) -> Result {
    fs::FsNativeSd fs;
    R_TRY(fs.GetFsOpenResult());
    fs.CreateDirectoryRecursively("/switch/hats-tools/cache");

    if (!pbox->ShouldExit()) {
        pbox->NewTransfer(i18n::Reorder("Downloading ", version));
        const auto result = curl::Api().ToFile(
            curl::Url{url},
            curl::Path{HATS_TOOLS_UPDATE_ZIP},
            curl::OnProgress{pbox->OnDownloadProgressCallback()}
        );
        R_UNLESS(result.success, 0x1);
    }

    ON_SCOPE_EXIT(fs.DeleteFile(HATS_TOOLS_UPDATE_ZIP));

    if (!pbox->ShouldExit()) {
        pbox->NewTransfer("Installing HATS Tools...");
        R_TRY(thread::TransferUnzipAll(pbox, HATS_TOOLS_UPDATE_ZIP, &fs, "/"));
        R_TRY(fs.Commit());
    }

    return 0;
}

bool ParseToolsUpdateInfo(const fs::FsPath& path, ToolsUpdateInfo* out) {
    auto doc = yyjson_read_file(path.s, YYJSON_READ_NOFLAG, nullptr, nullptr);
    R_UNLESS(doc, false);
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    auto root = yyjson_doc_get_root(doc);
    R_UNLESS(root && yyjson_is_obj(root), false);

    auto tag_val = yyjson_obj_get(root, "tag_name");
    const char* version = tag_val ? yyjson_get_str(tag_val) : nullptr;
    R_UNLESS(version, false);

    if (!App::IsVersionNewer(APP_VERSION, version)) {
        return false;
    }

    auto assets = yyjson_obj_get(root, "assets");
    R_UNLESS(assets && yyjson_is_arr(assets), false);

    const char* update_url = nullptr;
    size_t idx, max;
    yyjson_val* asset;
    yyjson_arr_foreach(assets, idx, max, asset) {
        if (!yyjson_is_obj(asset)) {
            continue;
        }

        auto name_val = yyjson_obj_get(asset, "name");
        auto url_val = yyjson_obj_get(asset, "browser_download_url");
        const char* name = name_val ? yyjson_get_str(name_val) : nullptr;
        const char* url = url_val ? yyjson_get_str(url_val) : nullptr;
        if (name && url && std::strstr(name, ".zip")) {
            update_url = url;
            break;
        }
    }
    R_UNLESS(update_url, false);

    out->version = version;
    out->url = update_url;
    return true;
}

void InstallToolsUpdate(const std::string& version, const std::string& url) {
    App::Push<ProgressBox>(0, "Updating HATS Tools"_i18n, version,
        [version, url](auto pbox) -> Result {
            return InstallToolsUpdateZip(pbox, url, version);
        },
        [](Result rc) {
            if (R_FAILED(rc)) {
                App::Push<ErrorBox>(rc, "Failed to update HATS Tools");
                return;
            }

            App::Push<OptionBox>(
                "HATS Tools updated successfully.\n\nRestart now?",
                "Later"_i18n, "Restart"_i18n, 1,
                [](auto op_index) {
                    if (op_index && *op_index == 1) {
                        App::ExitRestart();
                    }
                });
        });
}

void PromptToolsUpdate(const ToolsUpdateInfo& update) {
    App::Push<OptionBox>(
        "HATS Tools " + update.version + " is available.\n\nDownload and install it now?",
        "Later"_i18n, "Update"_i18n, 1,
        [update](auto op_index) {
            if (op_index && *op_index == 1) {
                InstallToolsUpdate(update.version, update.url);
            }
        });
}

} // namespace

void CheckHatsToolsUpdateManually() {
    auto update = std::make_shared<ToolsUpdateInfo>();
    App::Push<ProgressBox>(0, "Checking"_i18n, "HATS Tools update",
        [update](auto pbox) -> Result {
            pbox->NewTransfer("Checking latest HATS Tools...");
            const auto result = curl::Api().ToFile(
                curl::Url{HATS_TOOLS_RELEASE_URL},
                curl::Path{HATS_TOOLS_RELEASE_CACHE},
                curl::Header{
                    {"Accept", "application/vnd.github+json"},
                }
            );
            R_UNLESS(result.success, 0x1);
            R_UNLESS(ParseToolsUpdateInfo(result.path, update.get()), 0x2);
            return 0;
        },
        [update](Result rc) {
            if (rc == 0x2) {
                App::Push<OptionBox>("HATS Tools is already up to date."_i18n, "OK"_i18n);
                return;
            }
            if (R_FAILED(rc)) {
                App::Push<ErrorBox>(rc, "Failed to check HATS Tools update");
                return;
            }
            PromptToolsUpdate(*update);
        });
}

// Embedded icon data
constexpr const u8 ICON_HATS_PACK[]{
    #embed <icons/update-hats.png>
};

constexpr const u8 ICON_FIRMWARE[]{
    #embed <icons/update-firmware.png>
};

constexpr const u8 ICON_CHEATS[]{
    #embed <icons/cheats.png>
};

constexpr const u8 ICON_UNINSTALL[]{
    #embed <icons/component-manager.png>
};

constexpr const u8 ICON_FILE_BROWSER[]{
    #embed <icons/file-browser.png>
};

constexpr const u8 ICON_ADVANCED[]{
    #embed <icons/advanced-options.png>
};

constexpr const u8 ICON_APP_SHOP[]{
    #embed <icons/app-shop.png>
};

constexpr const u8 ICON_GAME_HUB[]{
    #embed <icons/game-hub.png>
};

constexpr const u8 ICON_WIPE_SYSMMC[]{
    #embed <icons/wipe-sysmmc.png>
};

constexpr const u8 ICON_CLEAN_EMUMMC[]{
    #embed <icons/clean-emummc.png>
};

constexpr const u8 ICON_NETWORK[]{
    #embed <icons/network.png>
};

MainMenu::MainMenu() : MenuBase{"HATS Tools " HATS_TOOLS_VERSION, MenuFlag_None} {
    // Initialize menu items with icon paths
    m_items = {
        {"Update HATS Pack", "Download and install HATS pack releases", "icons/update-hats.png"},
        {"Update Firmware", "Download, validate, and install system firmware", "icons/update-firmware.png"},
        {"Cheats", "Download cheat codes from nx-cheats-db", "icons/cheats.png"},
        {"Component Manager", "Disable, enable, or delete installed components", "icons/component-manager.png"},
        {"File Browser", "Browse and manage files on SD Card", "icons/file-browser.png"},
        {"Homebrew App Shop", "Download and update homebrew apps", "icons/app-shop.png"},
        {"Game Hub", "Manage installed games and game packages", "icons/game-hub.png"},
        {"Wipe sysMMC", "Reset sysMMC to factory default", "icons/wipe-sysmmc.png"},
        {"Network Tools", "Network, USB, and online tools", "icons/network.png"},
        {"Advanced Options", "Configure application settings including logging", "icons/advanced-options.png"}
    };

    // Refresh version info
    RefreshVersionInfo();

    // Load icons
    LoadIcons();

    // Set up actions
    this->SetActions(
        std::make_pair(Button::A, Action{"Select"_i18n, [this](){
            OnSelect();
        }}),
        std::make_pair(Button::B, Action{"Exit"_i18n, [this](){
            App::Exit();
        }}),
        std::make_pair(Button::START, Action{App::Exit})
    );

    // Set up two rows of icons, centered horizontally.
    // 5 icons * 174px + 4 gaps * 20px = 950px, left margin: 165px.
    const Vec2 pad{20, 20};  // Padding between cells
    const Vec4 v{165, 225.f, 174, 174};
    m_list = std::make_unique<List>(5, 10, m_pos, v, pad);
    m_list->SetLayout(List::Layout::GRID);

    CheckToolsUpdate();
}

MainMenu::~MainMenu() {
    // Clean up icon textures
    auto* vg = App::GetVg();
    if (vg) {
        if (m_clean_emummc_icon) {
            nvgDeleteImage(vg, m_clean_emummc_icon);
        }
        for (auto& item : m_items) {
            if (item.icon_texture) {
                nvgDeleteImage(vg, item.icon_texture);
                item.icon_texture = 0;
            }
        }
    }
}

void MainMenu::Update(Controller* controller, TouchInfo* touch) {
    if (!m_tools_update_prompted && !m_tools_update_checking && m_tools_update_retry_tick &&
        armTicksToNs(armGetSystemTick()) >= m_tools_update_retry_tick) {
        m_tools_update_retry_tick = 0;
        CheckToolsUpdate();
    }

    if (m_wipe_countdown_active) {
        if (controller->GotDown(Button::B)) {
            CancelWipeCountdown();
            return;
        }

        if (controller->GotDown(Button::A)) {
            RunWipeSysmmc();
            return;
        }

        if (controller->GotDown(Button::LEFT)) {
            m_wipe_countdown_seconds = std::max(WIPE_COUNTDOWN_MIN_SECONDS, m_wipe_countdown_seconds - 5);
            m_wipe_countdown_start_tick = armGetSystemTick();
            return;
        }

        if (controller->GotDown(Button::RIGHT)) {
            m_wipe_countdown_seconds = std::min(WIPE_COUNTDOWN_MAX_SECONDS, m_wipe_countdown_seconds + 5);
            m_wipe_countdown_start_tick = armGetSystemTick();
            return;
        }

        if (GetWipeCountdownSecondsRemaining() <= 0) {
            RunWipeSysmmc();
        }
        return;
    }

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

void MainMenu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    const float header_y = GetY() + 20.f;
    const float info_x = 80.f;

    // Draw version info header
    gfx::drawTextArgs(vg, info_x, header_y, 20.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "HATS: %s", m_hats_version.c_str());

    gfx::drawTextArgs(vg, info_x, header_y + 24.f, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
        theme->GetColour(ThemeEntryID_TEXT_INFO),
        "Firmware: %s | Atmosphere: %s",
        m_firmware_version.c_str(),
        m_atmosphere_version.c_str());

    // Draw separator
    gfx::drawRect(vg, 75.f, header_y + 55.f, 1220.f - 150.f, 1.f, theme->GetColour(ThemeEntryID_LINE));

    // Draw icon grid menu items (matching homebrew style)
    m_list->Draw(vg, theme, m_items.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& item = m_items[i];
        const bool selected = (m_index == i);

        // Draw background and selection
        if (selected) {
            gfx::drawRectOutline(vg, theme, 4.f, v);
        }

        // Draw icon (matching homebrew style: use full cell size)
        const auto texture = i == 7 && m_mmc_status == "EMUMMC" ? m_clean_emummc_icon : item.icon_texture;
        if (texture) {
            gfx::drawImage(vg, v, texture, 5); // 5px corner radius like homebrew
        }

        // Draw label at bottom (only when selected, like homebrew)
        if (selected) {
            gfx::drawAppLable(vg, theme, m_scroll_name, x, y, w, item.label.c_str());
        }
    });

    if (m_wipe_countdown_active) {
        DrawWipeCountdown(vg, theme);
    }
}

void MainMenu::OnFocusGained() {
    MenuBase::OnFocusGained();
    RefreshVersionInfo();
}

void MainMenu::SetIndex(s64 index) {
    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }
    m_scroll_name.Reset();
}

void MainMenu::OnSelect() {
    switch (m_index) {
        case 0: // Update HATS Pack
            App::Push<PackMenu>();
            break;
        case 1: // Update Firmware
            App::Push<OptionBox>(
                "Choose firmware source."_i18n,
                "GitHub (Online)"_i18n, "Local (Offline)"_i18n, 0,
                [](auto op_index) {
                    if (!op_index) {
                        return;
                    }

                    App::Push<FirmwareMenu>(*op_index == 1);
                });
            break;
        case 2: // Cheats
            App::Push<CheatsMenu>();
            break;
        case 3: // Component Manager
            App::Push<UninstallerMenu>();
            break;
        case 4: // File Browser
            App::Push<ui::menu::filebrowser::Menu>(MenuFlag_None);
            break;
        case 5: // Homebrew App Shop
            App::Push<ui::menu::appstore::Menu>(MenuFlag_None);
            break;
        case 6: // Game Hub
            App::Push<GameHubMenu>();
            break;
        case 8: // Network Tools
            App::Push<NetworkMenu>();
            break;
        case 7: // Wipe SYSMMC
            StartWipeSysmmcFlow();
            break;
        case 9: // Advanced Options
            App::Push<SettingsMenu>();
            break;
    }
}

void MainMenu::StartWipeSysmmcFlow() {
    bool is_emummc{};
    const auto rc = IsRunningEmummc(&is_emummc);
    if (R_FAILED(rc)) {
        App::PushErrorBox(rc, "Failed to verify SYSMMC/emuMMC status. Wipe blocked."_i18n);
        return;
    }

    if (is_emummc) {
        StartCleanEmummcFlow();
        return;
    }

    if (!hosversionAtLeast(3, 0, 0)) {
        App::Push<OptionBox>(
            "Wipe SYSMMC (OFW/Stock) requires HOS 3.0.0 or newer."_i18n,
            "OK"_i18n
        );
        return;
    }

    App::Push<OptionBox>(
        "This will wipe SYSMMC (OFW/Stock) and all data.\nData cannot be recovered.\nConsole will force restart."_i18n,
        "Cancel"_i18n,
        "Continue"_i18n,
        0,
        [this](auto op_index) {
            if (op_index && *op_index == 1) {
                StartWipeCountdown();
            }
        }
    );
}

void MainMenu::StartWipeCountdown() {
    m_wipe_countdown_active = true;
    m_wipe_countdown_seconds = 10;
    m_wipe_countdown_start_tick = armGetSystemTick();
}

void MainMenu::CancelWipeCountdown() {
    m_wipe_countdown_active = false;
    m_wipe_countdown_start_tick = 0;
    App::Notify("Wipe SYSMMC (OFW/Stock) canceled"_i18n);
}

void MainMenu::RunWipeSysmmc() {
    if (m_wipe_in_progress) {
        return;
    }

    m_wipe_in_progress = true;
    m_wipe_countdown_active = false;

    bool is_emummc{};
    Result rc = IsRunningEmummc(&is_emummc);
    if (R_FAILED(rc)) {
        m_wipe_in_progress = false;
        App::PushErrorBox(rc, "Failed to verify SYSMMC/emuMMC status. Wipe blocked."_i18n);
        return;
    }

    if (is_emummc) {
        m_wipe_in_progress = false;
        App::Push<OptionBox>(
            "Wipe SYSMMC (OFW/Stock) is blocked while running emuMMC.\nBoot SYSMMC CFW first."_i18n,
            "OK"_i18n
        );
        return;
    }

    rc = nsInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = nsResetToFactorySettingsForRefurbishment();
        nsExit();
    }

    m_wipe_in_progress = false;

    if (R_FAILED(rc)) {
        App::PushErrorBox(rc, "Failed to wipe SYSMMC (OFW/Stock)"_i18n);
        return;
    }

    App::Notify("SYSMMC (OFW/Stock) wiped. Rebooting..."_i18n);
    if (R_FAILED(utils::requestForcedReboot())) {
        App::Push<OptionBox>(
            "SYSMMC (OFW/Stock) wipe completed. You must reboot the console now."_i18n,
            "Reboot"_i18n,
            [](auto) {
                utils::requestForcedReboot();
            }
        );
    }
}

auto MainMenu::GetWipeCountdownSecondsRemaining() const -> s64 {
    return std::max<s64>(0, m_wipe_countdown_seconds - GetElapsedSeconds(m_wipe_countdown_start_tick));
}

void MainMenu::DrawWipeCountdown(NVGcontext* vg, Theme* theme) {
    const Vec4 box{
        (SCREEN_WIDTH / 2.f) - 385.f,
        (SCREEN_HEIGHT / 2.f) - 147.5f,
        770.f,
        295.f
    };
    const auto center_x = box.x + box.w / 2.f;
    const auto seconds = GetWipeCountdownSecondsRemaining();

    gfx::dimBackground(vg);
    gfx::drawRect(vg, box, theme->GetColour(ThemeEntryID_POPUP), 5);
    gfx::drawText(vg, center_x, box.y + 45.f, 26.f, theme->GetColour(ThemeEntryID_TEXT), "Wipe SYSMMC (OFW/Stock)"_i18n.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    gfx::drawTextBox(
        vg,
        box.x + 55.f,
        box.y + 105.f,
        22.f,
        box.w - 110.f,
        theme->GetColour(ThemeEntryID_TEXT),
        "Factory reset will start automatically if left untouched."_i18n.c_str(),
        NVG_ALIGN_CENTER | NVG_ALIGN_TOP
    );
    const auto seconds_text = std::to_string(seconds);
    gfx::drawText(vg, center_x, box.y + 165.f, 38.f, theme->GetColour(ThemeEntryID_TEXT_SELECTED), seconds_text.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    gfx::drawRect(vg, box.x, box.y + 220.f - 2.f, box.w, 2.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
    gfx::drawText(vg, box.x + 75.f, box.y + 245.f, 20.f, theme->GetColour(ThemeEntryID_TEXT_INFO), "B Cancel"_i18n.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    gfx::drawText(vg, center_x, box.y + 245.f, 20.f, theme->GetColour(ThemeEntryID_TEXT_INFO), "Left/Right Adjust"_i18n.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    gfx::drawText(vg, box.x + box.w - 75.f, box.y + 245.f, 20.f, theme->GetColour(ThemeEntryID_TEXT_INFO), "A Wipe Now"_i18n.c_str(), NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
}

void MainMenu::RefreshVersionInfo() {
    m_hats_version = sphaira::hats::getHatsVersion();
    m_firmware_version = sphaira::hats::getSystemFirmware();
    m_atmosphere_version = sphaira::hats::getAtmosphereVersion();
    m_mmc_status = "UNKNOWN";

    bool is_emummc{};
    if (R_SUCCEEDED(IsRunningEmummc(&is_emummc))) {
        m_mmc_status = is_emummc ? "EMUMMC" : "SYSMMC";
    }

    auto& wipe = m_items[7];
    if (m_mmc_status == "EMUMMC") {
        wipe.label = "Clean emuMMC";
        wipe.description = "Delete all user games and saves from emuMMC";
        wipe.icon_path = "icons/clean-emummc.png";
    } else if (m_mmc_status == "SYSMMC") {
        wipe.label = "Wipe sysMMC";
        wipe.description = "Reset sysMMC to factory default";
        wipe.icon_path = "icons/wipe-sysmmc.png";
    } else {
        wipe.label = "Wipe / Clean unavailable";
        wipe.description = "Unable to verify sysMMC/emuMMC; action blocked";
        wipe.icon_path = "icons/wipe-sysmmc.png";
    }

    SetTitle(std::string{"HATS Tools " HATS_TOOLS_VERSION " - "} + m_mmc_status);

    m_is_erista = sphaira::hats::isErista();
}

void MainMenu::LoadIcons() {
    auto* vg = App::GetVg();
    if (!vg) return;

    // Helper lambda to load icon from embedded data
    auto load_icon = [vg](const u8* data, size_t size, int& out_texture) -> bool {
        int width, height, channels;
        u8* decoded = stbi_load_from_memory(data, size, &width, &height, &channels, 4);
        if (!decoded) {
            log_write("Failed to load icon from memory\n");
            return false;
        }

        out_texture = nvgCreateImageRGBA(vg, width, height, 0, decoded);
        stbi_image_free(decoded);

        if (!out_texture) {
            log_write("Failed to create NanoVG image texture\n");
            return false;
        }

        return true;
    };

    // Load all icons
    bool success = true;
    success &= load_icon(ICON_HATS_PACK, sizeof(ICON_HATS_PACK), m_items[0].icon_texture);
    success &= load_icon(ICON_FIRMWARE, sizeof(ICON_FIRMWARE), m_items[1].icon_texture);
    success &= load_icon(ICON_CHEATS, sizeof(ICON_CHEATS), m_items[2].icon_texture);
    success &= load_icon(ICON_UNINSTALL, sizeof(ICON_UNINSTALL), m_items[3].icon_texture);
    success &= load_icon(ICON_FILE_BROWSER, sizeof(ICON_FILE_BROWSER), m_items[4].icon_texture);
    success &= load_icon(ICON_APP_SHOP, sizeof(ICON_APP_SHOP), m_items[5].icon_texture);
    success &= load_icon(ICON_GAME_HUB, sizeof(ICON_GAME_HUB), m_items[6].icon_texture);
    success &= load_icon(ICON_WIPE_SYSMMC, sizeof(ICON_WIPE_SYSMMC), m_items[7].icon_texture);
    success &= load_icon(ICON_CLEAN_EMUMMC, sizeof(ICON_CLEAN_EMUMMC), m_clean_emummc_icon);
    success &= load_icon(ICON_NETWORK, sizeof(ICON_NETWORK), m_items[8].icon_texture);
    success &= load_icon(ICON_ADVANCED, sizeof(ICON_ADVANCED), m_items[9].icon_texture);

    if (success) {
        log_write("Successfully loaded all menu icons\n");
    } else {
        log_write("Warning: Some icons failed to load\n");
    }
}

void MainMenu::CheckToolsUpdate() {
    if (m_tools_update_prompted || m_tools_update_checking) {
        return;
    }

    m_tools_update_checking = true;
    m_tools_update_attempts++;

    curl::Api().ToFileAsync(
        curl::Url{HATS_TOOLS_RELEASE_URL},
        curl::Path{HATS_TOOLS_RELEASE_CACHE},
        curl::Flags{curl::Flag_Cache},
        curl::StopToken{this->GetToken()},
        curl::Header{
            {"Accept", "application/vnd.github+json"},
        },
        curl::OnComplete{[this](auto& result) {
            m_tools_update_checking = false;

            if (!result.success) {
                hats_log_write("hats: failed to check HATS Tools update\n");
                ScheduleToolsUpdateRetry();
                return false;
            }

            ToolsUpdateInfo update;
            if (!ParseToolsUpdateInfo(result.path, &update)) {
                return true;
            }

            m_tools_update_prompted = true;
            PromptToolsUpdate(update);
            return true;
        }}
    );
}

void MainMenu::ScheduleToolsUpdateRetry() {
    const auto retry_index = m_tools_update_attempts - 1;
    constexpr int retry_count = sizeof(TOOLS_UPDATE_RETRY_DELAY_NS) / sizeof(TOOLS_UPDATE_RETRY_DELAY_NS[0]);
    if (retry_index < 0 || retry_index >= retry_count) {
        m_tools_update_retry_tick = 0;
        return;
    }

    m_tools_update_retry_tick = armTicksToNs(armGetSystemTick()) + TOOLS_UPDATE_RETRY_DELAY_NS[retry_index];
}

} // namespace sphaira::ui::menu::hats
