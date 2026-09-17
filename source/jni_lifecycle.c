/*
 * Copyright (C) 2026 Asphalt-5-Vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "jni_lifecycle.h"
#include "utils/logger.h"

#include <psp2/kernel/processmgr.h>

void impl_Asphalt5_Exit(jmethodID id, va_list args) {
    l_info("Asphalt5.Exit() called -- shutting down.");
    log_shutdown();
    sceKernelExitProcess(0);
    while (1);
}
