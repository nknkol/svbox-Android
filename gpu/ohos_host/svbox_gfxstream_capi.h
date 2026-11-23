/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SvboxGfxMode {
    SVBOX_GFX_MODE_SWIFTSHADER = 0,
    SVBOX_GFX_MODE_GLES = 1,
    SVBOX_GFX_MODE_VULKAN = 2,
} SvboxGfxMode;

typedef struct SvboxGfxStats {
    int initialized;
    int mode;
    int vulkanEnabled;
    int activeChannels;
    int activeProcesses;
    uint64_t framesPosted;
    int32_t fps;
    int32_t width;
    int32_t height;
    char vendor[128];
    char renderer[128];
    char version[128];
} SvboxGfxStats;

/* Initialize gfxstream host renderer and start AF_UNIX pipe server on sockPath.
 * Returns 0 on success, negative on error. */
int svbox_gfx_init(int32_t width, int32_t height, int mode, const char* sockPath);

/* Attach or resize the HarmonyOS OHNativeWindow (from XComponent). */
int svbox_gfx_set_window(void* ohNativeWindow, int32_t surfaceW, int32_t surfaceH);

/* Detach the current OHNativeWindow (e.g. OnSurfaceDestroyed). */
void svbox_gfx_clear_window(void);

/* Trigger a host repaint of the last posted framebuffer. */
void svbox_gfx_repaint(void);

/* Enable/disable active display presentation when backgrounded/paused. */
void svbox_gfx_set_visible(int visible);

/* Set target host VSync refresh rate (60 / 90 / 120 Hz). */
void svbox_gfx_set_vsync_hz(int32_t hz);

/* Stop socket server and shut down gfxstream host renderer cleanly. */
void svbox_gfx_stop(void);

/* Query live statistics and hardware GL/VK renderer strings. */
void svbox_gfx_get_stats(SvboxGfxStats* outStats);

#ifdef __cplusplus
}
#endif
