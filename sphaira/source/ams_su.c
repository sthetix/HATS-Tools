/* Atmosphere/Daybreak ams:su client. GPL-2.0-or-later. */
#include <switch.h>
#include <string.h>
#include "ams_su.h"
#include "service_guard.h"

static Service g_amssu_srv;
static TransferMemory g_tmem;
NX_GENERATE_SERVICE_GUARD(amssu);

Result _amssuInitialize(void) { return smGetService(&g_amssu_srv, "ams:su"); }
void _amssuCleanup(void) { serviceClose(&g_amssu_srv); tmemClose(&g_tmem); }

static void copy_path(char out[FS_MAX_PATH], const char* path) {
    strncpy(out, path, FS_MAX_PATH - 1);
    out[FS_MAX_PATH - 1] = 0;
}

Result amssuGetUpdateInformation(AmsSuUpdateInformation* out, const char* path) {
    char send_path[FS_MAX_PATH] = {0}; copy_path(send_path, path);
    return serviceDispatchOut(&g_amssu_srv, 0, *out,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize },
        .buffers = { { send_path, FS_MAX_PATH } });
}

Result amssuValidateUpdate(AmsSuUpdateValidationInfo* out, const char* path) {
    char send_path[FS_MAX_PATH] = {0}; copy_path(send_path, path);
    return serviceDispatchOut(&g_amssu_srv, 1, *out,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize },
        .buffers = { { send_path, FS_MAX_PATH } });
}

Result amssuSetupUpdate(void* buffer, size_t size, const char* path, bool exfat) {
    Result rc = buffer ? tmemCreateFromMemory(&g_tmem, buffer, size, Perm_None)
                       : tmemCreate(&g_tmem, size, Perm_None);
    if (R_FAILED(rc)) return rc;
    char send_path[FS_MAX_PATH] = {0}; copy_path(send_path, path);
    const struct { u8 exfat; u64 size; } in = { exfat, g_tmem.size };
    rc = serviceDispatchIn(&g_amssu_srv, 2, in,
        .in_num_handles = 1, .in_handles = { g_tmem.handle },
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize },
        .buffers = { { send_path, FS_MAX_PATH } });
    if (R_FAILED(rc)) tmemClose(&g_tmem);
    return rc;
}

Result amssuRequestPrepareUpdate(AsyncResult* out) {
    memset(out, 0, sizeof(*out));
    Handle event = INVALID_HANDLE;
    Result rc = serviceDispatch(&g_amssu_srv, 4, .out_num_objects = 1,
        .out_objects = &out->s, .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = &event);
    if (R_SUCCEEDED(rc)) eventLoadRemote(&out->event, event, false);
    return rc;
}

Result amssuGetPrepareUpdateProgress(NsSystemUpdateProgress* out) {
    return serviceDispatchOut(&g_amssu_srv, 5, *out);
}
Result amssuHasPreparedUpdate(bool* out) {
    u8 value = 0; Result rc = serviceDispatchOut(&g_amssu_srv, 6, value);
    if (R_SUCCEEDED(rc) && out) *out = value & 1;
    return rc;
}
Result amssuApplyPreparedUpdate(void) { return serviceDispatch(&g_amssu_srv, 7); }
