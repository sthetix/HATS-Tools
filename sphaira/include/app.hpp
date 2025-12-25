#pragma once

#include "nanovg.h"
#include "nanovg/dk_renderer.hpp"
#include "ui/widget.hpp"
#include "ui/notification.hpp"
#include "owo.hpp"
#include "option.hpp"
#include "fs.hpp"
#include "log.hpp"
#include "utils/audio.hpp"

#ifdef USE_NVJPG
#include <nvjpg.hpp>
#endif
#include <switch.h>
#include <vector>
#include <string>
#include <span>
#include <optional>
#include <utility>

namespace sphaira {

using SoundEffect = audio::SoundEffect;

enum class LaunchType {
    Normal,
    Forwader_Unknown,
    Forwader_Sphaira,
};

struct AmsEmummcPaths {
    char file_based_path[0x80];
    char nintendo[0x80];
};

// todo: why is this global???
void DrawElement(float x, float y, float w, float h, ThemeEntryID id);
void DrawElement(const Vec4&, ThemeEntryID id);

class App {
public:
    App(const char* argv0);
    ~App();
    void Loop();

    static App* GetApp();

    static void Exit();
    static void ExitRestart();
    static auto GetVg() -> NVGcontext*;

    static void Push(std::unique_ptr<ui::Widget>&&);

    template<ui::DerivedFromWidget T, typename... Args>
    static void Push(Args&&... args) {
        Push(std::make_unique<T>(std::forward<Args>(args)...));
    }

    // pops all widgets above a menu
    static void PopToMenu();

    // this is thread safe
    static void Notify(const std::string& text, ui::NotifEntry::Side side = ui::NotifEntry::Side::RIGHT);
    static void Notify(ui::NotifEntry entry);
    static void NotifyPop(ui::NotifEntry::Side side = ui::NotifEntry::Side::RIGHT);
    static void NotifyClear(ui::NotifEntry::Side side = ui::NotifEntry::Side::RIGHT);
    static void NotifyFlashLed();

    // if R_FAILED(rc), pushes error box. returns rc passed in.
    static Result PushErrorBox(Result rc, const std::string& message);

    static auto GetThemeMetaList() -> std::span<ThemeMeta>;
    static void SetTheme(s64 theme_index);
    static auto GetThemeIndex() -> s64;

    static auto GetDefaultImage() -> int;
    static auto GetDefaultImageData() -> std::span<const u8>;

    // returns argv[0]
    static auto GetExePath() -> fs::FsPath;
    // returns true if we are hbmenu.
    static auto IsHbmenu() -> bool;

    static auto GetLogEnable() -> bool;
    static auto GetReplaceHbmenuEnable() -> bool;
    static auto GetThemeMusicEnable() -> bool;
    static auto GetTextScrollSpeed() -> long;

    // NSZ compression options
    static auto GetNszCompressLevel() -> long;
    static auto GetNszThreadCount() -> long;
    static auto GetNszBlockExponent() -> long;

    static void SetLogEnable(bool enable);
    static void SetReplaceHbmenuEnable(bool enable);
    static void SetThemeMusicEnable(bool enable);
    static void SetTextScrollSpeed(long index);

    static void PlaySoundEffect(SoundEffect effect);

    static void DisplayMenuOptions(bool left_side = true);
    static void DisplayAdvancedOptions(bool left_side = true);

    void Draw();
    void Update();
    void Poll();

    // void DrawElement(float x, float y, float w, float h, ui::ThemeEntryID id);
    auto LoadElementImage(std::string_view value) -> ElementEntry;
    auto LoadElementColour(std::string_view value) -> ElementEntry;
    auto LoadElement(std::string_view data, ElementType type) -> ElementEntry;

    void LoadTheme(const ThemeMeta& meta);
    void CloseTheme();
    void CloseThemeBackgroundMusic();
    void ScanThemes(const std::string& path);
    void ScanThemeEntries();
    void LoadAndPlayThemeMusic();
    static Result SetDefaultBackgroundMusic(fs::Fs* fs, const fs::FsPath& path);
    static void SetBackgroundMusicPause(bool pause);

    static Result GetSdSize(s64* free, s64* total);
    static Result GetEmmcSize(s64* free, s64* total);

    // helper that converts 1.2.3 to a u32 used for comparisons.
    static auto GetVersionFromString(const char* str) -> u32;
    static auto IsVersionNewer(const char* current, const char* new_version) -> u32;

    static auto IsApplication() -> bool {
        const auto type = appletGetAppletType();
        return type == AppletType_Application || type == AppletType_SystemApplication;
    }

    static auto IsApplet() -> bool {
        return !IsApplication();
    }

    // returns true if launched in applet mode with a title suspended in the background.
    static auto IsAppletWithSuspendedApp() -> bool {
        R_UNLESS(IsApplet(), false);
        R_TRY_RESULT(pmdmntInitialize(), false);
        ON_SCOPE_EXIT(pmdmntExit());

        u64 pid;
        return R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid));
    }

    static auto IsEmummc() -> bool;
    static auto IsParitionBaseEmummc() -> bool;
    static auto IsFileBaseEmummc() -> bool;

    static void SetAutoSleepDisabled(bool enable) {
        static Mutex mutex{};
        static int ref_count{};

        mutexLock(&mutex);
        ON_SCOPE_EXIT(mutexUnlock(&mutex));

        if (enable) {
            appletSetAutoSleepDisabled(true);
            ref_count++;
        } else {
            if (ref_count) {
                ref_count--;
            }

            if (!ref_count) {
                appletSetAutoSleepDisabled(false);
            }
        }
    }

    static void SetBoostMode(bool enable, bool force = false) {
        static Mutex mutex{};
        static int ref_count{};

        mutexLock(&mutex);
        ON_SCOPE_EXIT(mutexUnlock(&mutex));

        if (enable) {
            ref_count++;
            appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
        } else {
            if (ref_count) {
                ref_count--;
            }
        }

        if (!ref_count || force) {
            ref_count = 0;
            appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        }
    }

    static auto GetAccountList() -> std::vector<AccountProfileBase> {
        std::vector<AccountProfileBase> out;

        AccountUid uids[ACC_USER_LIST_SIZE];
        s32 account_count;
        if (R_SUCCEEDED(accountListAllUsers(uids, std::size(uids), &account_count))) {
            for (s32 i = 0; i < account_count; i++) {
                AccountProfile profile;
                if (R_SUCCEEDED(accountGetProfile(&profile, uids[i]))) {
                    ON_SCOPE_EXIT(accountProfileClose(&profile));

                    AccountProfileBase base;
                    if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base))) {
                        // sometimes the uid for the acc can differ to the base.
                        base.uid = uids[i];
                        log_write("[ACC] found uid: 0x%016lX%016lX\n", uids[i].uid[0], uids[i].uid[1]);
                        log_write("[ACC] base  uid: 0x%016lX%016lX\n", base.uid.uid[0], base.uid.uid[1]);
                        out.emplace_back(base);
                    }
                }
            }
        }

        return out;
    }

// private:
    static constexpr inline auto CONFIG_PATH = "/config/hats-tools/config.ini";
    static constexpr inline auto PLAYLOG_PATH = "/config/hats-tools/playlog.ini";
    static constexpr inline auto INI_SECTION = "config";
    static constexpr inline auto DEFAULT_THEME_PATH = "romfs:/themes/default_theme.ini";

    fs::FsPath m_app_path;
    u64 m_start_timestamp{};
    int m_default_image{};

    bool m_is_launched_via_sphaira_forwader{};

    NVGcontext* vg{};
    PadState m_pad{};
    TouchInfo m_touch_info{};
    Controller m_controller{};
    KeyboardState m_keyboard{};
    std::vector<ThemeMeta> m_theme_meta_entries;

    Vec2 m_scale{1, 1};

    std::vector<std::unique_ptr<ui::Widget>> m_widgets;
    u32 m_pop_count{};
    ui::NotifMananger m_notif_manager{};

    AppletHookCookie m_appletHookCookie{};

    Theme m_theme{};
    fs::FsPath theme_path{};
    s64 m_theme_index{};

    AmsEmummcPaths m_emummc_paths{};
    bool m_quit{};

    option::OptionBool m_log_enabled{INI_SECTION, "log_enabled", false};
    option::OptionBool m_replace_hbmenu{INI_SECTION, "replace_hbmenu", false};
    option::OptionString m_default_music{INI_SECTION, "default_music", "/config/hats-tools/themes/default_music.bfstm"};
    option::OptionString m_theme_path{INI_SECTION, "theme", DEFAULT_THEME_PATH};
    option::OptionBool m_theme_music{INI_SECTION, "theme_music", true};
    option::OptionString m_center_menu{INI_SECTION, "center_side_menu", "Homebrew"};
    option::OptionString m_left_menu{INI_SECTION, "left_side_menu", "FileBrowser"};
    option::OptionString m_right_menu{INI_SECTION, "right_side_menu", "Appstore"};
    option::OptionBool m_progress_boost_mode{INI_SECTION, "progress_boost_mode", true};

    // todo: move this into it's own menu
    option::OptionLong m_text_scroll_speed{"accessibility", "text_scroll_speed", 1}; // normal

    // YATI installation options (kept for NSP/NSZ installation via FileBrowser)
    option::OptionBool m_install_sd{INI_SECTION, "install_sd", true};
    option::OptionBool m_allow_downgrade{INI_SECTION, "allow_downgrade", false};
    option::OptionBool m_skip_if_already_installed{INI_SECTION, "skip_if_already_installed", true};
    option::OptionBool m_ticket_only{INI_SECTION, "ticket_only", false};
    option::OptionBool m_skip_base{INI_SECTION, "skip_base", false};
    option::OptionBool m_skip_patch{INI_SECTION, "skip_patch", false};
    option::OptionBool m_skip_addon{INI_SECTION, "skip_addon", false};
    option::OptionBool m_skip_data_patch{INI_SECTION, "skip_data_patch", false};
    option::OptionBool m_skip_ticket{INI_SECTION, "skip_ticket", false};
    option::OptionBool m_skip_nca_hash_verify{INI_SECTION, "skip_nca_hash_verify", false};
    option::OptionBool m_skip_rsa_header_fixed_key_verify{INI_SECTION, "skip_rsa_header_fixed_key_verify", false};
    option::OptionBool m_skip_rsa_npdm_fixed_key_verify{INI_SECTION, "skip_rsa_npdm_fixed_key_verify", false};
    option::OptionBool m_ignore_distribution_bit{INI_SECTION, "ignore_distribution_bit", false};
    option::OptionBool m_convert_to_common_ticket{INI_SECTION, "convert_to_common_ticket", false};
    option::OptionBool m_convert_to_standard_crypto{INI_SECTION, "convert_to_standard_crypto", false};
    option::OptionLong m_lower_master_key{INI_SECTION, "lower_master_key", 0};
    option::OptionLong m_lower_system_version{INI_SECTION, "lower_system_version", 0};

    // NSZ compression options (for NSZ export via FileBrowser)
    option::OptionLong m_nsz_compress_level{"dump", "nsz_compress_level", 3};
    option::OptionLong m_nsz_compress_threads{"dump", "nsz_compress_threads", 3};
    option::OptionBool m_nsz_compress_ldm{"dump", "nsz_compress_ldm", false};
    option::OptionLong m_nsz_compress_block{"dump", "nsz_compress_block", 20};
    option::OptionLong m_nsz_compress_block_exponent{"dump", "nsz_compress_block_exponent", 20};

    // HATS options.
    option::OptionString m_hats_installer_payload{"hats", "installer_payload", "/bootloader/payloads/hats-installer.bin"};
    option::OptionString m_hats_staging_path{"hats", "staging_path", "/hats-staging"};
    option::OptionString m_hats_install_mode{"hats", "install_mode", "default"}; // replace, default, clean

    std::shared_ptr<fs::FsNativeSd> m_fs{};
    audio::SongID m_background_music{};

#ifdef USE_NVJPG
    nj::Decoder m_decoder;
#endif

    double m_delta_time{};

private: // from nanovg decko3d example by adubbz
    static constexpr unsigned NumFramebuffers = 2;
    static constexpr unsigned StaticCmdSize = 0x1000;
    unsigned s_width{1280};
    unsigned s_height{720};
    dk::UniqueDevice device;
    dk::UniqueQueue queue;
    std::optional<CMemPool> pool_images;
    std::optional<CMemPool> pool_code;
    std::optional<CMemPool> pool_data;
    dk::UniqueCmdBuf cmdbuf;
    CMemPool::Handle depthBuffer_mem;
    CMemPool::Handle framebuffers_mem[NumFramebuffers];
    dk::Image depthBuffer;
    dk::Image framebuffers[NumFramebuffers];
    DkCmdList framebuffer_cmdlists[NumFramebuffers];
    dk::UniqueSwapchain swapchain;
    DkCmdList render_cmdlist;
    std::optional<nvg::DkRenderer> renderer;
    void createFramebufferResources();
    void destroyFramebufferResources();
    void recordStaticCommands();
};

} // namespace sphaira
