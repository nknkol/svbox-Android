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

#define SVBOX_GFX_MAX_DISPLAYS 9

typedef struct SvboxGfxDisplayStats {
    int32_t displaySlot;
    int32_t attached;
    int32_t visible;
    int32_t width;
    int32_t height;
    uint32_t lastCb;
    uint64_t framesPosted;
    int32_t fps;
    uint32_t topBarColor; /* 0xAARRGGBB sampled from top edge (0 if unavailable) */
} SvboxGfxDisplayStats;

/* Initialize gfxstream host renderer and start AF_UNIX pipe server on sockPath.
 * Also starts a secondary-display zero-copy present server on <sockPath>.present.
 * Returns 0 on success, negative on error. */
int svbox_gfx_init(int32_t width, int32_t height, int mode, const char* sockPath);

/* Attach or resize the primary (slot 0) HarmonyOS OHNativeWindow (from XComponent). */
int svbox_gfx_set_window(void* ohNativeWindow, int32_t surfaceW, int32_t surfaceH);

/* Detach the primary (slot 0) OHNativeWindow (e.g. OnSurfaceDestroyed). */
void svbox_gfx_clear_window(void);

/* Attach or resize an OHNativeWindow for any displaySlot [0..SVBOX_GFX_MAX_DISPLAYS-1].
 * Slot 0 routes to the primary RenderWindow; slots 1..8 use independent DisplayVk swapchains. */
int svbox_gfx_set_display_window(int32_t displaySlot, void* ohNativeWindow,
                                 int32_t surfaceW, int32_t surfaceH);

/* Detach the OHNativeWindow bound to displaySlot [0..SVBOX_GFX_MAX_DISPLAYS-1]. */
void svbox_gfx_clear_display_window(int32_t displaySlot);

/* Enable/disable presentation for a specific displaySlot. */
void svbox_gfx_set_display_visible(int32_t displaySlot, int visible);

/* Present a guest ColorBuffer handle zero-copy to displaySlot [1..SVBOX_GFX_MAX_DISPLAYS-1].
 * Returns 0 on success (or headless recorded), negative on error. */
int svbox_gfx_present_cb(int32_t displaySlot, uint32_t cbHandle, int32_t width, int32_t height);

/* Query per-display frame stats. */
void svbox_gfx_get_display_stats(int32_t displaySlot, SvboxGfxDisplayStats* outStats);

/* On-demand diagnostic RGBA8888 readback of the latest ColorBuffer for displaySlot [1..8].
 * Zero readback occurs during normal rendering. Returns 0 on success. */
int svbox_gfx_read_display_rgba(int32_t displaySlot, void* outRgba, uint32_t bufBytes,
                                int32_t* outW, int32_t* outH);

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
