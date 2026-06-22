/*
 * Atmosphere system-update service client, derived from Daybreak.
 * Copyright (c) Atmosphere-NX. GPL-2.0-or-later.
 */
#pragma once
#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u32 version;
    bool exfat_supported;
    u32 num_firmware_variations;
    u32 firmware_variation_ids[16];
} AmsSuUpdateInformation;

typedef struct {
    Result result;
    Result exfat_result;
    NcmContentMetaKey invalid_key;
    NcmContentId invalid_content_id;
} AmsSuUpdateValidationInfo;

Result amssuInitialize(void);
void amssuExit(void);
Result amssuGetUpdateInformation(AmsSuUpdateInformation* out, const char* path);
Result amssuValidateUpdate(AmsSuUpdateValidationInfo* out, const char* path);
Result amssuSetupUpdate(void* buffer, size_t size, const char* path, bool exfat);
Result amssuRequestPrepareUpdate(AsyncResult* out);
Result amssuGetPrepareUpdateProgress(NsSystemUpdateProgress* out);
Result amssuHasPreparedUpdate(bool* out);
Result amssuApplyPreparedUpdate(void);

#ifdef __cplusplus
}
#endif
