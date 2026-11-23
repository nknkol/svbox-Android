/* SPDX-License-Identifier: MIT */
/* Unified logging macro for ohos_bridge: uses HarmonyOS HiLog in HAP builds
 * and stderr in standalone CLI builds. */
#pragma once

#ifdef OHOS_BRIDGE_HEADLESS
#include <cstdio>
#define OB_LOG_INFO(tag, fmt, ...)  std::fprintf(stderr, "[I/" tag "] " fmt "\n", ##__VA_ARGS__)
#define OB_LOG_WARN(tag, fmt, ...)  std::fprintf(stderr, "[W/" tag "] " fmt "\n", ##__VA_ARGS__)
#define OB_LOG_ERROR(tag, fmt, ...) std::fprintf(stderr, "[E/" tag "] " fmt "\n", ##__VA_ARGS__)
#else
#include <hilog/log.h>
#define OB_LOG_INFO(tag, fmt, ...)  OH_LOG_Print(LOG_APP, LOG_INFO, 0x0, tag, fmt, ##__VA_ARGS__)
#define OB_LOG_WARN(tag, fmt, ...)  OH_LOG_Print(LOG_APP, LOG_WARN, 0x0, tag, fmt, ##__VA_ARGS__)
#define OB_LOG_ERROR(tag, fmt, ...) OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0, tag, fmt, ##__VA_ARGS__)
#endif
