/*
 * HATS Installer Payload
 * Minimal payload to install HATS pack files outside of Horizon OS
 *
 * Based on TegraExplorer/hekate by CTCaer, naehrwert, shchmue
 */

#ifndef VERSION
#define VERSION "1.0.1"
#endif

#include <string.h>

#include <utils/ini.h>
#include <display/di.h>
#include "gfx.h"
#include <libs/fatfs/ff.h>
#include <mem/heap.h>
#include <mem/minerva.h>
#include <power/max77620.h>
#include <soc/bpmp.h>
#include <soc/fuse.h>
#include <soc/hw_init.h>
#include <soc/pmc.h>
#include <soc/t210.h>
#include "nx_sd.h"
#include <storage/sdmmc.h>
#include <utils/btn.h>
#include <utils/util.h>
#include <utils/sprintf.h>

#include "fs.h"

// Configuration
#define STAGING_PATH      "sd:/hats-staging"
#define PAYLOAD_PATH      "sd:/payload.bin"
#define HEKATE_INI_BAK   "sd:/bootloader/hekate_ipl.ini.bak"
#define HEKATE_INI       "sd:/bootloader/hekate_ipl.ini"
#define CONFIG_PATH       "sd:/config/hats-tools/config.ini"
#define ATMOSPHERE_PATH   "sd:/atmosphere"
#define BOOTLOADER_PATH   "sd:/bootloader"
#define SWITCH_PATH       "sd:/switch"

// Install modes
typedef enum {
    MODE_OVERWRITE     = 0,  // Only overwrite, no deletion
    MODE_REPLACE_AMS   = 1,  // Delete atmosphere only
    MODE_REPLACE_AMS_BL= 2,  // Delete atmosphere and bootloader
    MODE_CLEAN         = 3   // Delete atmosphere, bootloader, switch
} install_mode_t;

static const char *mode_names[] = {
    "overwrite",
    "replace_ams",
    "replace_ams_bl",
    "clean"
};

// Payload launch defines
#define RELOC_META_OFF      0x7C
#define PATCHED_RELOC_SZ    0x94
#define PATCHED_RELOC_STACK 0x40007000
#define PATCHED_RELOC_ENTRY 0x40010000
#define EXT_PAYLOAD_ADDR    0xC0000000
#define RCM_PAYLOAD_ADDR    (EXT_PAYLOAD_ADDR + ALIGN(PATCHED_RELOC_SZ, 0x10))
#define COREBOOT_END_ADDR   0xD0000000

// Hekate config structure (simplified)
typedef struct _hekate_config
{
    u32 autoboot;
    u32 autoboot_list;
    u32 bootwait;
    u32 backlight;
    u32 autohosoff;
    u32 autonogc;
    u32 updater2p;
    u32 bootprotect;
    bool t210b01;
    bool se_keygen_done;
    bool sept_run;
    bool aes_slots_new;
    bool emummc_force_disable;
    bool rcm_patched;
    u32  errors;
} hekate_config;

// Boot config structures (required by bdk)
hekate_config h_cfg;
boot_cfg_t __attribute__((section ("._boot_cfg"))) b_cfg;
volatile nyx_storage_t *nyx_str = (nyx_storage_t *)NYX_STORAGE_ADDR;

static void *coreboot_addr;
static int total_errors = 0;
static install_mode_t install_mode = MODE_OVERWRITE;  // Default to [overwrite] mode (safest)

// Use BDK colors (already defined in types.h)
#define COLOR_CYAN    0xFF00FFFF
#define COLOR_WHITE   0xFFFFFFFF
#define COLOR_ORANGE  0xFF00A5FF

static void set_default_configuration(void)
{
    h_cfg.autoboot = 0;
    h_cfg.autoboot_list = 0;
    h_cfg.bootwait = 3;
    h_cfg.se_keygen_done = 0;
    h_cfg.backlight = 100;
    h_cfg.autohosoff = 0;
    h_cfg.autonogc = 1;
    h_cfg.updater2p = 0;
    h_cfg.bootprotect = 0;
    h_cfg.errors = 0;
    h_cfg.sept_run = 0;
    h_cfg.aes_slots_new = false;
    h_cfg.rcm_patched = fuse_check_patched_rcm();
    h_cfg.emummc_force_disable = false;
    h_cfg.t210b01 = hw_get_chip_id() == GP_HIDREV_MAJOR_T210B01;
}

static void set_color(u32 color) {
    gfx_con_setcol(color, gfx_con.fillbg, gfx_con.bgcol);
}

static void print_header(void) {
    gfx_clear_grey(0x1B);
    gfx_con_setpos(0, 0);
    set_color(COLOR_CYAN);
    gfx_printf("========================================\n");
    gfx_printf("    HATS Installer Payload v%s\n", VERSION);
    gfx_printf("========================================\n\n");
    set_color(COLOR_WHITE);
}

static void print_result(const char *action, int result) {
    if (result == 0) {
        set_color(COLOR_GREEN);
        gfx_printf("[OK] %s\n", action);
        // log_write("[OK] %s\n", action);
    } else {
        set_color(COLOR_RED);
        gfx_printf("[FAIL] %s (err=%d)\n", action, result);
        // log_write("[FAIL] %s - Error %d: %s\n", action, result, fs_error_str(result));
        total_errors++;
    }
    set_color(COLOR_WHITE);
}

void reloc_patcher(u32 payload_dst, u32 payload_src, u32 payload_size) {
    memcpy((u8 *)payload_src, (u8 *)IPL_LOAD_ADDR, PATCHED_RELOC_SZ);

    volatile reloc_meta_t *relocator = (reloc_meta_t *)(payload_src + RELOC_META_OFF);

    relocator->start = payload_dst - ALIGN(PATCHED_RELOC_SZ, 0x10);
    relocator->stack = PATCHED_RELOC_STACK;
    relocator->end   = payload_dst + payload_size;
    relocator->ep    = payload_dst;

    if (payload_size == 0x7000) {
        memcpy((u8 *)(payload_src + ALIGN(PATCHED_RELOC_SZ, 0x10)), coreboot_addr, 0x7000);
    }
}

static int launch_payload(const char *path) {
    if (!path)
        return 1;

    if (sd_mount()) {
        FIL fp;
        if (f_open(&fp, path, FA_READ)) {
            gfx_printf("Payload not found: %s\n", path);
            return 1;
        }

        void *buf;
        u32 size = f_size(&fp);

        if (size < 0x30000)
            buf = (void *)RCM_PAYLOAD_ADDR;
        else {
            coreboot_addr = (void *)(COREBOOT_END_ADDR - size);
            buf = coreboot_addr;
        }

        if (f_read(&fp, buf, size, NULL)) {
            f_close(&fp);
            return 1;
        }

        f_close(&fp);
        sd_unmount();

        if (size < 0x30000) {
            reloc_patcher(PATCHED_RELOC_ENTRY, EXT_PAYLOAD_ADDR, ALIGN(size, 0x10));
            hw_reinit_workaround(false, byte_swap_32(*(u32 *)(buf + size - sizeof(u32))));
        } else {
            reloc_patcher(PATCHED_RELOC_ENTRY, EXT_PAYLOAD_ADDR, 0x7000);
            hw_reinit_workaround(true, 0);
        }

        sdmmc_storage_init_wait_sd();

        void (*ext_payload_ptr)() = (void *)EXT_PAYLOAD_ADDR;
        (*ext_payload_ptr)();
    }

    return 1;
}

static int file_exists(const char *path) {
    FILINFO fno;
    return (f_stat(path, &fno) == FR_OK);
}

// Backup original hekate_ipl.ini and create temporary one for HATS autoboot
static void setup_hekate_ini_backup(void) {
    FIL fp_src;
    FIL fp_bak;
    FIL fp_ini;
    UINT bytes_read;
    UINT bytes_written;
    u8 *buf;

    // 1. Backup original hekate_ipl.ini to .bak
    if (f_open(&fp_src, HEKATE_INI, FA_READ) == FR_OK) {
        u32 file_size = f_size(&fp_src);
        buf = (u8 *)malloc(file_size);
        if (buf) {
            if (f_read(&fp_src, buf, file_size, &bytes_read) == FR_OK && bytes_read == file_size) {
                f_close(&fp_src);
                if (f_open(&fp_bak, HEKATE_INI_BAK, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
                    f_write(&fp_bak, buf, file_size, &bytes_written);
                    f_close(&fp_bak);
                }
                free(buf);
            } else {
                f_close(&fp_src);
                free(buf);
                buf = NULL;
            }
        } else {
            f_close(&fp_src);
        }
    } else {
        buf = NULL;
    }

    // 2. Create new temporary hekate_ipl.ini with HATS autoboot config
    const char *temp_ini =
        "[config]\n"
        "autoboot=1\n"
        "autoboot_list=0\n"
        "bootwait=0\n"
        "verification=1\n"
        "backlight=100\n"
        "autohosoff=2\n"
        "autonogc=1\n"
        "updater2p=1\n"
        "\n"
        "[HATS Installer]\n"
        "payload=/bootloader/payloads/hats-installer.bin\n";

    if (f_open(&fp_ini, HEKATE_INI, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        u32 len = strlen(temp_ini);
        f_write(&fp_ini, temp_ini, len, &bytes_written);
        f_close(&fp_ini);
    }
}

// Restore hekate_ipl.ini from backup (called after installation)
static bool restore_hekate_ini(void) {
    FIL fp_bak;
    FIL fp_dst;
    UINT bytes_read;
    UINT bytes_written;
    u8 *buf;

    if (f_open(&fp_bak, HEKATE_INI_BAK, FA_READ) != FR_OK) {
        return false;
    }

    u32 bak_size = f_size(&fp_bak);
    if (bak_size == 0) {
        f_close(&fp_bak);
        f_unlink(HEKATE_INI_BAK);
        return false;
    }

    buf = (u8 *)malloc(bak_size);
    if (buf == NULL) {
        f_close(&fp_bak);
        return false;
    }

    if (f_read(&fp_bak, buf, bak_size, &bytes_read) != FR_OK || bytes_read != bak_size) {
        f_close(&fp_bak);
        free(buf);
        return false;
    }
    f_close(&fp_bak);

    // Overwrite hekate_ipl.ini with backup content
    bool success = false;
    if (f_open(&fp_dst, HEKATE_INI, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        if (f_write(&fp_dst, buf, bak_size, &bytes_written) == FR_OK && bytes_written == bak_size) {
            success = true;
        }
        f_close(&fp_dst);
    }

    free(buf);
    f_unlink(HEKATE_INI_BAK);
    return success;
}

// Parse config.ini to get install mode
static void parse_config(void) {
    link_t config_list;
    list_init(&config_list);

    // log_write("\n--- Parsing config ---\n");

    if (!ini_parse(&config_list, (char *)CONFIG_PATH, false)) {
        set_color(COLOR_ORANGE);
        gfx_printf("No config found, using [overwrite] mode\n");
        set_color(COLOR_WHITE);
        // log_write("Config not found: %s\n", CONFIG_PATH);
        // log_write("Using default mode\n");
        install_mode = MODE_OVERWRITE;
        return;
    }

    // Look for [installer] section and install_mode key
    LIST_FOREACH_ENTRY(ini_sec_t, sec, &config_list, link) {
        if (sec->type != INI_CHOICE)
            continue;

        if (strcmp(sec->name, "installer") == 0) {
            LIST_FOREACH_ENTRY(ini_kv_t, kv, &sec->kvs, link) {
                if (strcmp(kv->key, "install_mode") == 0) {
                    // Parse the value
                    for (int i = 0; i < 4; i++) {
                        if (strcmp(kv->val, mode_names[i]) == 0) {
                            install_mode = (install_mode_t)i;
                            set_color(COLOR_CYAN);
                            gfx_printf("Config mode: [%s]\n", mode_names[i]);
                            set_color(COLOR_WHITE);
                            // log_write("Mode set to: %s\n", mode_names[i]);
                            goto cleanup;
                        }
                    }
                    // Value found but not recognized
                    break;
                }
            }
            // Found [installer] section but no valid install_mode
            break;
        }
    }

    set_color(COLOR_ORANGE);
    gfx_printf("No valid mode in config, using [overwrite]\n");
    set_color(COLOR_WHITE);
    // log_write("No valid mode found, using default\n");
    install_mode = MODE_OVERWRITE;

cleanup:
    // Free ini memory - iterate safely using manual approach
    link_t *iter, *safe;
    LIST_FOREACH_SAFE(iter, &config_list) {
        ini_sec_t *sec = CONTAINER_OF(iter, ini_sec_t, link);
        if (sec->type == INI_CHOICE) {
            link_t *kv_iter, *kv_safe;
            LIST_FOREACH_SAFE(kv_iter, &sec->kvs) {
                ini_kv_t *kv = CONTAINER_OF(kv_iter, ini_kv_t, link);
                if (kv->key) free(kv->key);
                if (kv->val) free(kv->val);
                free(kv);
            }
        }
        if (sec->name) free(sec->name);
        free(sec);
    }
}

// Find and delete HATS-*.txt version file in root
static void delete_hats_txt(void) {
    DIR dir;
    FILINFO fno;
    int res;
    int found = 0;

    // log_write("\n--- Deleting HATS-*.txt ---\n");

    res = f_opendir(&dir, "sd:/");
    if (res != FR_OK) {
        // log_write("Could not open root dir: %s\n", fs_error_str(res));
        return;
    }

    while (1) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;

        // Skip directories
        if (fno.fattrib & AM_DIR) continue;

        // Check if filename starts with "HATS-" and ends with ".txt"
        if (strncmp(fno.fname, "HATS-", 5) == 0) {
            size_t len = strlen(fno.fname);
            if (len > 4 && strcmp(fno.fname + len - 4, ".txt") == 0) {
                char path[64];
                s_printf(path, "sd:/%s", fno.fname);
                // log_write("Found: %s\n", fno.fname);
                res = f_unlink(path);
                if (res == FR_OK) {
                    set_color(COLOR_GREEN);
                    gfx_printf("  Deleted: %s\n", fno.fname);
                    // log_write("  Deleted OK\n");
                    found++;
                } else {
                    set_color(COLOR_RED);
                    gfx_printf("  Failed: %s\n", fno.fname);
                    // log_write("  ERROR: %s\n", fs_error_str(res));
                    total_errors++;
                }
                set_color(COLOR_WHITE);
            }
        }
    }

    f_closedir(&dir);

    if (found == 0) {
        set_color(COLOR_ORANGE);
        gfx_printf("  No HATS-*.txt found\n");
        set_color(COLOR_WHITE);
        // log_write("No HATS-*.txt found\n");
    }
}

// Helper to draw a progress bar
static void draw_progress_bar(u32 start_x, u32 start_y, int percent, const char *current_file) {
    int i;
    const int bar_width = 20;
    char display_name[26];

    // Truncate filename if too long
    int len = strlen(current_file);
    if (len > 24) {
        strncpy(display_name, current_file, 21);
        strcpy(display_name + 21, "...");
        display_name[24] = 0;
    } else {
        strcpy(display_name, current_file);
    }

    // Clear area by rewriting background
    gfx_con_setpos(start_x, start_y);
    set_color(COLOR_WHITE);
    gfx_printf("  %s                    ", display_name); // Clear with spaces
    gfx_con_setpos(start_x, start_y + 1);
    gfx_printf("  [");

    // Draw progress bar
    int filled = (percent * bar_width) / 100;
    for (i = 0; i < bar_width; i++) {
        if (i < filled) {
            set_color(0xFF00FF00); // Green fill
            gfx_printf("|");
        } else {
            set_color(0xFF444444); // Dark empty
            gfx_printf("-");
        }
    }
    set_color(COLOR_WHITE);
    gfx_printf("] %3d%%", percent);
}

// Copy all contents from staging directory to SD root
static int copy_staging_contents(void) {
    DIR dir;
    FILINFO fno;
    int res;
    int copied = 0;
    int total = 0;
    int last_percent = -1;
    u32 prog_start_x, prog_start_y;

    // log_write("\n--- Copying staging contents ---\n");

    // First, count total items
    res = f_opendir(&dir, STAGING_PATH);
    if (res != FR_OK) {
        gfx_printf("  ERROR: Cannot open staging\n");
        // log_write("ERROR: Cannot open staging dir: %s\n", fs_error_str(res));
        return res;
    }

    while (1) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;
        total++;
    }
    f_closedir(&dir);

    if (total == 0) {
        gfx_printf("  No files to copy\n");
        return FR_OK;
    }

    // Save starting position for progress display
    gfx_con_getpos(&prog_start_x, &prog_start_y);

    // Now copy each item
    res = f_opendir(&dir, STAGING_PATH);
    if (res != FR_OK) {
        gfx_printf("\n  ERROR: Cannot open staging\n");
        // log_write("ERROR: Cannot open staging dir: %s\n", fs_error_str(res));
        return res;
    }

    while (1) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;

        char src_path[256];
        char dst_path[256];
        s_printf(src_path, "%s/%s", STAGING_PATH, fno.fname);
        s_printf(dst_path, "sd:/%s", fno.fname);

        // log_write("Copying: %s\n", fno.fname);

        if (fno.fattrib & AM_DIR) {
            res = folder_copy(src_path, "sd:/");
        } else {
            res = file_copy(src_path, dst_path);
        }

        if (res != FR_OK) {
            set_color(COLOR_RED);
            gfx_printf("\n  ERROR: %s\n", fno.fname);
            // log_write("ERROR copying %s: %s\n", fno.fname, fs_error_str(res));
            set_color(COLOR_WHITE);
            total_errors++;
        } else {
            copied++;
        }

        // Update progress display
        int percent = (copied * 100) / total;
        if (percent != last_percent) {
            draw_progress_bar(prog_start_x, prog_start_y, percent, fno.fname);
            last_percent = percent;
        }
    }

    f_closedir(&dir);

    set_color(COLOR_GREEN);
    gfx_printf("\n  Done! %d/%d items copied\n", copied, total);
    set_color(COLOR_WHITE);
    // log_write("Total: %d/%d items copied\n", copied, total);

    return (copied == total) ? FR_OK : FR_DISK_ERR;
}

static void do_install(void) {
    int res;

    // Step 1: Delete folders based on mode
    set_color(COLOR_YELLOW);
    gfx_printf("Step 1: Cleanup (mode: [%s])...\n", mode_names[install_mode]);
    set_color(COLOR_WHITE);
    // log_write("\n--- Step 1: Cleanup (mode: %s) ---\n", mode_names[install_mode]);

    if (install_mode == MODE_OVERWRITE) {
        set_color(COLOR_CYAN);
        gfx_printf("  Mode: overwrite - skipping deletions\n");
        set_color(COLOR_WHITE);
        // log_write("Overwrite mode: no deletions\n");
    } else {
        // Delete atmosphere (all modes except overwrite)
        if (file_exists(ATMOSPHERE_PATH)) {
            gfx_printf("  Deleting /atmosphere...\n");
            res = folder_delete(ATMOSPHERE_PATH);
            print_result("/atmosphere", res);
        } else {
            set_color(COLOR_ORANGE);
            gfx_printf("  [SKIP] /atmosphere\n");
            set_color(COLOR_WHITE);
            // log_write("[SKIP] /atmosphere not found\n");
        }

        // MODE_REPLACE_AMS_BL and MODE_CLEAN also delete bootloader
        if (install_mode == MODE_REPLACE_AMS_BL || install_mode == MODE_CLEAN) {
            if (file_exists(BOOTLOADER_PATH)) {
                gfx_printf("  Deleting /bootloader...\n");
                res = folder_delete(BOOTLOADER_PATH);
                print_result("/bootloader", res);
            } else {
                set_color(COLOR_ORANGE);
                gfx_printf("  [SKIP] /bootloader\n");
                set_color(COLOR_WHITE);
                // log_write("[SKIP] /bootloader not found\n");
            }
        }

        // MODE_CLEAN also deletes switch
        if (install_mode == MODE_CLEAN) {
            if (file_exists(SWITCH_PATH)) {
                gfx_printf("  Deleting /switch...\n");
                res = folder_delete(SWITCH_PATH);
                print_result("/switch", res);
            } else {
                set_color(COLOR_ORANGE);
                gfx_printf("  [SKIP] /switch\n");
                set_color(COLOR_WHITE);
                // log_write("[SKIP] /switch not found\n");
            }
        }
    }

    // Step 2: Delete HATS-*.txt file (always done)
    set_color(COLOR_YELLOW);
    gfx_printf("\nStep 2: Removing HATS version file...\n");
    set_color(COLOR_WHITE);
    // log_write("\n--- Step 2: Removing HATS version file ---\n");
    delete_hats_txt();

    // Step 3: Copy everything from staging to root
    set_color(COLOR_YELLOW);
    gfx_printf("\nStep 3: Copying from staging...\n");
    set_color(COLOR_WHITE);
    // log_write("\n--- Step 3: Copying from staging ---\n");

    copy_staging_contents();

    // Step 4: Cleanup staging folder
    set_color(COLOR_YELLOW);
    gfx_printf("\nStep 4: Cleaning up staging folder...\n");
    set_color(COLOR_WHITE);
    // log_write("\n--- Step 4: Cleaning up staging folder ---\n");

    res = folder_delete(STAGING_PATH);
    print_result("staging", res);

    // Summary
    gfx_printf("\n");
    // log_write("\n--- Summary ---\n");
    if (total_errors == 0) {
        set_color(COLOR_GREEN);
        gfx_printf("========================================\n");
        gfx_printf("    Installation Complete!\n");
        gfx_printf("========================================\n");
        // log_write("Installation completed successfully!\n");
    } else {
        set_color(COLOR_RED);
        gfx_printf("========================================\n");
        gfx_printf("    Installation Finished\n");
        gfx_printf("    %d error(s)\n", total_errors);
        gfx_printf("========================================\n");
        // log_write("Installation finished with %d error(s)\n", total_errors);
    }
    set_color(COLOR_WHITE);
}

extern void pivot_stack(u32 stack_top);

void ipl_main(void) {
    // Hardware initialization
    hw_init();
    pivot_stack(IPL_STACK_TOP);
    heap_init(IPL_HEAP_START);

    // Set bootloader's default configuration
    set_default_configuration();

    // Mount SD Card
    if (!sd_mount()) {
        // Can't show error without display, just reboot
        power_set_state(POWER_OFF_REBOOT);
    }

    // Backup original hekate_ipl.ini and create temporary config for HATS
    setup_hekate_ini_backup();

    // Logging disabled
    // log_init(LOG_PATH);

    // Initialize minerva for faster memory
    minerva_init();
    minerva_change_freq(FREQ_800);

    // Initialize display
    display_init();
    u32 *fb = display_init_framebuffer_pitch();
    gfx_init_ctxt(fb, 720, 1280, 720);
    gfx_con_init();
    display_backlight_pwm_init();
    display_backlight_brightness(100, 1000);

    // Overclock BPMP
    bpmp_clk_rate_set(BPMP_CLK_DEFAULT_BOOST);

    print_header();

    // Parse config.ini for install mode
    parse_config();

    // Check if staging directory exists
    if (!file_exists(STAGING_PATH)) {
        set_color(COLOR_RED);
        gfx_printf("No staging directory found!\n");
        gfx_printf("%s\n\n", STAGING_PATH);
        set_color(COLOR_WHITE);
        // log_write("ERROR: Staging directory not found: %s\n", STAGING_PATH);
        // log_close();

        // Launch payload if available
        if (file_exists(PAYLOAD_PATH)) {
            gfx_printf("Launching payload...\n");
            msleep(2000);
            launch_payload(PAYLOAD_PATH);
        }

        gfx_printf("Rebooting in 3 seconds...\n");
        msleep(3000);
        power_set_state(POWER_OFF_REBOOT);
    }

    // Show ready message
    set_color(COLOR_GREEN);
    gfx_printf("Staging found! Starting install...\n\n");
    set_color(COLOR_WHITE);
    // log_write("Staging directory found\n");

    // Perform the installation
    do_install();

    // Restore hekate_ipl.ini from backup after installation completes
    if (restore_hekate_ini()) {
        set_color(COLOR_GREEN);
        gfx_printf("\n[OK] hekate_ipl.ini restored\n");
        set_color(COLOR_WHITE);
    }

    // Logging disabled
    // log_close();

    // Launch payload after installation
    gfx_printf("\nLaunching payload in 3 seconds...\n");
    msleep(3000);

    if (file_exists(PAYLOAD_PATH)) {
        launch_payload(PAYLOAD_PATH);
    } else {
        // Payload not found, show error
        set_color(COLOR_RED);
        gfx_printf("\nERROR: payload.bin not found!\n");
        gfx_printf("Path: %s\n", PAYLOAD_PATH);
        set_color(COLOR_WHITE);
        msleep(3000);
        power_set_state(POWER_OFF_REBOOT);
    }

    // Should never reach here
    while (1)
        bpmp_halt();
}
