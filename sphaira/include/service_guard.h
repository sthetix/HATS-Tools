#pragma once
#include <switch/types.h>
#include <switch/result.h>
#include <switch/kernel/mutex.h>

typedef struct ServiceGuard {
    Mutex mutex;
    u32 refCount;
} ServiceGuard;

NX_INLINE bool serviceGuardBeginInit(ServiceGuard* guard) {
    mutexLock(&guard->mutex);
    return (guard->refCount++) == 0;
}

NX_INLINE Result serviceGuardEndInit(ServiceGuard* guard, Result rc, void (*cleanup)(void)) {
    if (R_FAILED(rc)) {
        cleanup();
        --guard->refCount;
    }
    mutexUnlock(&guard->mutex);
    return rc;
}

NX_INLINE void serviceGuardExit(ServiceGuard* guard, void (*cleanup)(void)) {
    mutexLock(&guard->mutex);
    if (guard->refCount && --guard->refCount == 0) cleanup();
    mutexUnlock(&guard->mutex);
}

#define NX_GENERATE_SERVICE_GUARD_PARAMS(name, paramdecl, parampass) \
    static ServiceGuard g_##name##Guard; \
    NX_INLINE Result _##name##Initialize paramdecl; \
    static void _##name##Cleanup(void); \
    Result name##Initialize paramdecl { \
        Result rc = 0; \
        if (serviceGuardBeginInit(&g_##name##Guard)) rc = _##name##Initialize parampass; \
        return serviceGuardEndInit(&g_##name##Guard, rc, _##name##Cleanup); \
    } \
    void name##Exit(void) { serviceGuardExit(&g_##name##Guard, _##name##Cleanup); }

#define NX_GENERATE_SERVICE_GUARD(name) NX_GENERATE_SERVICE_GUARD_PARAMS(name, (void), ())
