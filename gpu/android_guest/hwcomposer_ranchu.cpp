/* SPDX-License-Identifier: MIT */
/*
 * hwcomposer_ranchu.cpp - Minimal zero-readback HWC2 HAL for svbox-Android gfxstream.
 *
 * Forces all layers to HWC2_COMPOSITION_CLIENT so SurfaceFlinger composites on
 * the host GPU (Maleoon 916 via gfxstream), then presents the clientTarget
 * ColorBuffer handle directly to host FrameBuffer::post() via rcFBPost().
 */

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <cutils/properties.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer2.h>
#include <log/log.h>
#include <sync/sync.h>
#include <unistd.h>

#include "HostConnection.h"
#include "gralloc_cb_bp.h"

#undef LOG_TAG
#define LOG_TAG "hwcomposer_ranchu"

namespace {

constexpr hwc2_display_t kPrimaryDisplayId = 0;
constexpr uint32_t kNumConfigs = 3;
constexpr int kConfigFps[kNumConfigs] = {60, 90, 120};
constexpr int64_t kConfigVsyncNs[kNumConfigs] = {
    1000000000LL / 60,
    1000000000LL / 90,
    1000000000LL / 120,
};

hwc2_config_t configForFps(int fps) {
    if (fps >= 105) return 2;
    if (fps >= 75) return 1;
    return 0;
}

int readIntProp(const char* name, int defaultVal) {
    char buf[PROPERTY_VALUE_MAX] = {};
    if (property_get(name, buf, "") > 0) {
        char* end = nullptr;
        long v = strtol(buf, &end, 10);
        if (end && end != buf && v > 0) {
            return static_cast<int>(v);
        }
    }
    return defaultVal;
}

struct LayerState {
    hwc2_layer_t id = 0;
    int32_t compositionType = HWC2_COMPOSITION_INVALID;
};

class RanchuHwc2Device {
public:
    hwc2_device_t base{};

    RanchuHwc2Device() {
        width_ = readIntProp("ro.boot.redroid_width", 720);
        height_ = readIntProp("ro.boot.redroid_height", 1280);
        int reqFps = readIntProp("ro.boot.redroid_fps", 60);
        hwc2_config_t initCfg = configForFps(reqFps);
        activeConfig_.store(initCfg);
        fps_.store(kConfigFps[initCfg]);
        vsyncPeriodNs_.store(kConfigVsyncNs[initCfg]);
        dpi_ = readIntProp("ro.boot.redroid_dpi", readIntProp("ro.sf.lcd_density", 320));

        ALOGI("RanchuHwc2Device init: %dx%d @ %dHz (cfg=%u, %d dpi, vsyncPeriod=%lld ns, modes=60/90/120Hz)",
              width_, height_, fps_.load(), initCfg, dpi_,
              static_cast<long long>(vsyncPeriodNs_.load()));

        vsyncThread_ = std::thread(&RanchuHwc2Device::vsyncLoop, this);
    }

    ~RanchuHwc2Device() {
        {
            std::lock_guard<std::mutex> lk(vsyncMutex_);
            stopVsync_ = true;
            vsyncEnabled_ = false;
        }
        vsyncCv_.notify_all();
        if (vsyncThread_.joinable()) {
            vsyncThread_.join();
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (clientTargetFence_ >= 0) {
            close(clientTargetFence_);
            clientTargetFence_ = -1;
        }
    }

    static RanchuHwc2Device* from(hwc2_device_t* dev) {
        return reinterpret_cast<RanchuHwc2Device*>(dev);
    }

    // ---- Callbacks ----
    int32_t registerCallback(int32_t descriptor, hwc2_callback_data_t data,
                             hwc2_function_pointer_t pointer) {
        switch (descriptor) {
            case HWC2_CALLBACK_HOTPLUG: {
                HWC2_PFN_HOTPLUG fn = reinterpret_cast<HWC2_PFN_HOTPLUG>(pointer);
                {
                    std::lock_guard<std::mutex> lk(callbackMutex_);
                    hotplugData_ = data;
                    hotplugFn_ = fn;
                }
                if (fn) {
                    ALOGI("Sending initial HOTPLUG_CONNECTED for display %" PRIu64,
                          kPrimaryDisplayId);
                    fn(data, kPrimaryDisplayId, HWC2_CONNECTION_CONNECTED);
                }
                return HWC2_ERROR_NONE;
            }
            case HWC2_CALLBACK_REFRESH: {
                std::lock_guard<std::mutex> lk(callbackMutex_);
                refreshData_ = data;
                refreshFn_ = reinterpret_cast<HWC2_PFN_REFRESH>(pointer);
                return HWC2_ERROR_NONE;
            }
            case HWC2_CALLBACK_VSYNC: {
                std::lock_guard<std::mutex> lk(callbackMutex_);
                vsyncData_ = data;
                vsyncFn_ = reinterpret_cast<HWC2_PFN_VSYNC>(pointer);
                return HWC2_ERROR_NONE;
            }
            default:
                return HWC2_ERROR_BAD_PARAMETER;
        }
    }

    // ---- Layers ----
    int32_t createLayer(hwc2_display_t display, hwc2_layer_t* outLayer) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        if (!outLayer) return HWC2_ERROR_BAD_PARAMETER;
        std::lock_guard<std::mutex> lk(mutex_);
        hwc2_layer_t id = nextLayerId_++;
        layers_[id] = LayerState{id, HWC2_COMPOSITION_INVALID};
        *outLayer = id;
        return HWC2_ERROR_NONE;
    }

    int32_t destroyLayer(hwc2_display_t display, hwc2_layer_t layer) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = layers_.find(layer);
        if (it == layers_.end()) return HWC2_ERROR_BAD_LAYER;
        layers_.erase(it);
        return HWC2_ERROR_NONE;
    }

    int32_t setLayerCompositionType(hwc2_display_t display, hwc2_layer_t layer,
                                    int32_t type) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = layers_.find(layer);
        if (it == layers_.end()) return HWC2_ERROR_BAD_LAYER;
        it->second.compositionType = type;
        return HWC2_ERROR_NONE;
    }

    int32_t setLayerBuffer(hwc2_display_t display, hwc2_layer_t layer,
                           buffer_handle_t /*buffer*/, int32_t acquireFence) {
        if (acquireFence >= 0) {
            close(acquireFence);
        }
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        std::lock_guard<std::mutex> lk(mutex_);
        if (layers_.find(layer) == layers_.end()) return HWC2_ERROR_BAD_LAYER;
        return HWC2_ERROR_NONE;
    }

    // ---- Display config & state ----
    int32_t getDisplayConfigs(hwc2_display_t display, uint32_t* outNumConfigs,
                              hwc2_config_t* outConfigs) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        if (!outNumConfigs) return HWC2_ERROR_BAD_PARAMETER;
        if (!outConfigs) {
            *outNumConfigs = kNumConfigs;
        } else {
            uint32_t n = (*outNumConfigs < kNumConfigs) ? *outNumConfigs : kNumConfigs;
            for (uint32_t i = 0; i < n; ++i) {
                outConfigs[i] = i;
            }
            *outNumConfigs = n;
        }
        return HWC2_ERROR_NONE;
    }

    int32_t getActiveConfig(hwc2_display_t display, hwc2_config_t* outConfig) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        if (!outConfig) return HWC2_ERROR_BAD_PARAMETER;
        *outConfig = activeConfig_.load();
        return HWC2_ERROR_NONE;
    }

    int32_t setActiveConfig(hwc2_display_t display, hwc2_config_t config) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        if (config >= kNumConfigs) return HWC2_ERROR_BAD_CONFIG;
        {
            std::lock_guard<std::mutex> lk(vsyncMutex_);
            activeConfig_.store(config);
            fps_.store(kConfigFps[config]);
            vsyncPeriodNs_.store(kConfigVsyncNs[config]);
        }
        vsyncCv_.notify_all();
        ALOGI("setActiveConfig: display=%" PRIu64 " config=%u -> %dHz (vsyncPeriod=%lld ns)",
              display, config, kConfigFps[config], static_cast<long long>(kConfigVsyncNs[config]));
        return HWC2_ERROR_NONE;
    }

    int32_t getDisplayAttribute(hwc2_display_t display, hwc2_config_t config,
                                int32_t attribute, int32_t* outValue) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        if (config >= kNumConfigs) return HWC2_ERROR_BAD_CONFIG;
        if (!outValue) return HWC2_ERROR_BAD_PARAMETER;
        switch (attribute) {
            case HWC2_ATTRIBUTE_WIDTH:
                *outValue = width_;
                return HWC2_ERROR_NONE;
            case HWC2_ATTRIBUTE_HEIGHT:
                *outValue = height_;
                return HWC2_ERROR_NONE;
            case HWC2_ATTRIBUTE_VSYNC_PERIOD:
                *outValue = static_cast<int32_t>(kConfigVsyncNs[config]);
                return HWC2_ERROR_NONE;
            case HWC2_ATTRIBUTE_DPI_X:
            case HWC2_ATTRIBUTE_DPI_Y:
                *outValue = dpi_ * 1000;
                return HWC2_ERROR_NONE;
            case 6: // HWC2_ATTRIBUTE_CONFIG_GROUP (same resolution group -> seamless switch)
                *outValue = 0;
                return HWC2_ERROR_NONE;
            default:
                *outValue = -1;
                return HWC2_ERROR_BAD_PARAMETER;
        }
    }

    int32_t getDisplayName(hwc2_display_t display, uint32_t* outSize, char* outName) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        static constexpr char kName[] = "svbox-gfxstream";
        const uint32_t len = static_cast<uint32_t>(sizeof(kName) - 1);
        if (!outName) {
            *outSize = len;
        } else {
            uint32_t copyLen = (*outSize < len) ? *outSize : len;
            memcpy(outName, kName, copyLen);
            *outSize = copyLen;
        }
        return HWC2_ERROR_NONE;
    }

    int32_t getDisplayType(hwc2_display_t display, int32_t* outType) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        if (!outType) return HWC2_ERROR_BAD_PARAMETER;
        *outType = HWC2_DISPLAY_TYPE_PHYSICAL;
        return HWC2_ERROR_NONE;
    }

    int32_t setVsyncEnabled(hwc2_display_t display, int32_t enabled) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        if (enabled != HWC2_VSYNC_ENABLE && enabled != HWC2_VSYNC_DISABLE) {
            return HWC2_ERROR_BAD_PARAMETER;
        }
        {
            std::lock_guard<std::mutex> lk(vsyncMutex_);
            vsyncEnabled_ = (enabled == HWC2_VSYNC_ENABLE);
        }
        vsyncCv_.notify_all();
        return HWC2_ERROR_NONE;
    }

    // ---- Composition & Present ----
    int32_t setClientTarget(hwc2_display_t display, buffer_handle_t target,
                            int32_t acquireFence, int32_t /*dataspace*/,
                            hwc_region_t /*damage*/) {
        if (display != kPrimaryDisplayId) {
            if (acquireFence >= 0) close(acquireFence);
            return HWC2_ERROR_BAD_DISPLAY;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (clientTargetFence_ >= 0) {
            close(clientTargetFence_);
            clientTargetFence_ = -1;
        }
        clientTarget_ = target;
        clientTargetFence_ = acquireFence;
        return HWC2_ERROR_NONE;
    }

    int32_t validateDisplay(hwc2_display_t display, uint32_t* outNumTypes,
                            uint32_t* outNumRequests) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        std::lock_guard<std::mutex> lk(mutex_);
        changedLayers_.clear();
        for (auto& kv : layers_) {
            if (kv.second.compositionType != HWC2_COMPOSITION_CLIENT) {
                changedLayers_.push_back(kv.first);
            }
        }
        if (outNumTypes) *outNumTypes = static_cast<uint32_t>(changedLayers_.size());
        if (outNumRequests) *outNumRequests = 0;
        return changedLayers_.empty() ? HWC2_ERROR_NONE : HWC2_ERROR_HAS_CHANGES;
    }

    int32_t getChangedCompositionTypes(hwc2_display_t display, uint32_t* outNumElements,
                                       hwc2_layer_t* outLayers, int32_t* outTypes) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        if (!outNumElements) return HWC2_ERROR_BAD_PARAMETER;
        std::lock_guard<std::mutex> lk(mutex_);
        const uint32_t total = static_cast<uint32_t>(changedLayers_.size());
        if (!outLayers || !outTypes) {
            *outNumElements = total;
            return HWC2_ERROR_NONE;
        }
        const uint32_t count = (*outNumElements < total) ? *outNumElements : total;
        for (uint32_t i = 0; i < count; ++i) {
            outLayers[i] = changedLayers_[i];
            outTypes[i] = HWC2_COMPOSITION_CLIENT;
        }
        *outNumElements = count;
        return HWC2_ERROR_NONE;
    }

    int32_t acceptDisplayChanges(hwc2_display_t display) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        std::lock_guard<std::mutex> lk(mutex_);
        for (hwc2_layer_t id : changedLayers_) {
            auto it = layers_.find(id);
            if (it != layers_.end()) {
                it->second.compositionType = HWC2_COMPOSITION_CLIENT;
            }
        }
        changedLayers_.clear();
        return HWC2_ERROR_NONE;
    }

    int32_t presentDisplay(hwc2_display_t display, int32_t* outPresentFence) {
        if (display != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
        if (outPresentFence) *outPresentFence = -1;

        buffer_handle_t target = nullptr;
        int32_t fence = -1;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            target = clientTarget_;
            fence = clientTargetFence_;
            clientTargetFence_ = -1;
        }

        if (fence >= 0) {
            sync_wait(fence, 3000);
            close(fence);
        }

        if (!target) {
            return HWC2_ERROR_NONE;
        }

        HostConnection* hostCon = HostConnection::get();
        if (!hostCon) {
            ALOGE("presentDisplay: HostConnection::get() returned null");
            return HWC2_ERROR_NO_RESOURCES;
        }
        ExtendedRCEncoderContext* rcEnc = hostCon->rcEncoder();
        if (!rcEnc) {
            ALOGE("presentDisplay: rcEncoder() returned null");
            return HWC2_ERROR_NO_RESOURCES;
        }

        const uint32_t hostHandle = hostCon->grallocHelper()->getHostHandle(target);
        if (hostHandle == 0) {
            ALOGW("presentDisplay: clientTarget has hostHandle == 0");
            return HWC2_ERROR_NONE;
        }

        hostCon->lock();
        rcEnc->rcFBPost(rcEnc, hostHandle);
        hostCon->flush();
        hostCon->unlock();

        uint64_t count = ++presentCount_;
        if (count == 1 || count == 10 || (count % 300 == 0)) {
            ALOGI("presentDisplay: posted frame #%" PRIu64 " (cb=0x%x)", count, hostHandle);
        }
        return HWC2_ERROR_NONE;
    }

    void dump(uint32_t* outSize, char* outBuffer) {
        char info[256];
        int len = snprintf(info, sizeof(info),
                           "svbox-gfxstream hwcomposer.ranchu %dx%d@%dHz (cfg=%u) frames=%" PRIu64 "\n",
                           width_, height_, fps_.load(), activeConfig_.load(), presentCount_.load());
        if (len < 0) len = 0;
        if (!outBuffer) {
            *outSize = static_cast<uint32_t>(len);
        } else {
            uint32_t copyLen = (*outSize < static_cast<uint32_t>(len))
                                   ? *outSize
                                   : static_cast<uint32_t>(len);
            memcpy(outBuffer, info, copyLen);
            *outSize = copyLen;
        }
    }

private:
    void vsyncLoop() {
        struct timespec nextTick{};
        clock_gettime(CLOCK_MONOTONIC, &nextTick);

        while (true) {
            {
                std::unique_lock<std::mutex> lk(vsyncMutex_);
                vsyncCv_.wait(lk, [this] { return stopVsync_ || vsyncEnabled_; });
                if (stopVsync_) return;
                clock_gettime(CLOCK_MONOTONIC, &nextTick);
            }

            while (true) {
                int64_t period = vsyncPeriodNs_.load();
                nextTick.tv_nsec += period;
                while (nextTick.tv_nsec >= 1000000000LL) {
                    nextTick.tv_nsec -= 1000000000LL;
                    nextTick.tv_sec += 1;
                }
                while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &nextTick, nullptr) == EINTR) {
                }

                {
                    std::lock_guard<std::mutex> lk(vsyncMutex_);
                    if (stopVsync_) return;
                    if (!vsyncEnabled_) break;
                }

                HWC2_PFN_VSYNC fn = nullptr;
                hwc2_callback_data_t data = nullptr;
                {
                    std::lock_guard<std::mutex> lk(callbackMutex_);
                    fn = vsyncFn_;
                    data = vsyncData_;
                }
                if (fn) {
                    struct timespec now{};
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    int64_t ts = static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
                    fn(data, kPrimaryDisplayId, ts);
                }
            }
        }
    }

    int width_ = 720;
    int height_ = 1280;
    std::atomic<hwc2_config_t> activeConfig_{0};
    std::atomic<int> fps_{60};
    int dpi_ = 320;
    std::atomic<int64_t> vsyncPeriodNs_{16666666LL};

    std::mutex mutex_;
    hwc2_layer_t nextLayerId_ = 1;
    std::unordered_map<hwc2_layer_t, LayerState> layers_;
    std::vector<hwc2_layer_t> changedLayers_;
    buffer_handle_t clientTarget_ = nullptr;
    int32_t clientTargetFence_ = -1;
    std::atomic<uint64_t> presentCount_{0};

    std::mutex callbackMutex_;
    hwc2_callback_data_t hotplugData_ = nullptr;
    HWC2_PFN_HOTPLUG hotplugFn_ = nullptr;
    hwc2_callback_data_t refreshData_ = nullptr;
    HWC2_PFN_REFRESH refreshFn_ = nullptr;
    hwc2_callback_data_t vsyncData_ = nullptr;
    HWC2_PFN_VSYNC vsyncFn_ = nullptr;

    std::mutex vsyncMutex_;
    std::condition_variable vsyncCv_;
    bool vsyncEnabled_ = false;
    bool stopVsync_ = false;
    std::thread vsyncThread_;
};

// ---- C Trampolines ----
int32_t fnAcceptDisplayChanges(hwc2_device_t* dev, hwc2_display_t d) {
    return RanchuHwc2Device::from(dev)->acceptDisplayChanges(d);
}
int32_t fnCreateLayer(hwc2_device_t* dev, hwc2_display_t d, hwc2_layer_t* l) {
    return RanchuHwc2Device::from(dev)->createLayer(d, l);
}
int32_t fnCreateVirtualDisplay(hwc2_device_t*, uint32_t, uint32_t, int32_t*, hwc2_display_t*) {
    return HWC2_ERROR_NO_RESOURCES;
}
int32_t fnDestroyLayer(hwc2_device_t* dev, hwc2_display_t d, hwc2_layer_t l) {
    return RanchuHwc2Device::from(dev)->destroyLayer(d, l);
}
int32_t fnDestroyVirtualDisplay(hwc2_device_t*, hwc2_display_t) {
    return HWC2_ERROR_BAD_DISPLAY;
}
void fnDump(hwc2_device_t* dev, uint32_t* outSize, char* outBuf) {
    RanchuHwc2Device::from(dev)->dump(outSize, outBuf);
}
int32_t fnGetActiveConfig(hwc2_device_t* dev, hwc2_display_t d, hwc2_config_t* c) {
    return RanchuHwc2Device::from(dev)->getActiveConfig(d, c);
}
int32_t fnGetChangedCompositionTypes(hwc2_device_t* dev, hwc2_display_t d,
                                     uint32_t* n, hwc2_layer_t* l, int32_t* t) {
    return RanchuHwc2Device::from(dev)->getChangedCompositionTypes(d, n, l, t);
}
int32_t fnGetClientTargetSupport(hwc2_device_t*, hwc2_display_t d, uint32_t, uint32_t,
                                 int32_t, int32_t) {
    return (d == kPrimaryDisplayId) ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_DISPLAY;
}
int32_t fnGetColorModes(hwc2_device_t*, hwc2_display_t d, uint32_t* n, int32_t* modes) {
    if (d != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
    if (!modes) {
        *n = 1;
    } else if (*n >= 1) {
        modes[0] = HAL_COLOR_MODE_NATIVE;
        *n = 1;
    }
    return HWC2_ERROR_NONE;
}
int32_t fnGetDisplayAttribute(hwc2_device_t* dev, hwc2_display_t d, hwc2_config_t c,
                              int32_t attr, int32_t* v) {
    return RanchuHwc2Device::from(dev)->getDisplayAttribute(d, c, attr, v);
}
int32_t fnGetDisplayConfigs(hwc2_device_t* dev, hwc2_display_t d, uint32_t* n, hwc2_config_t* c) {
    return RanchuHwc2Device::from(dev)->getDisplayConfigs(d, n, c);
}
int32_t fnGetDisplayName(hwc2_device_t* dev, hwc2_display_t d, uint32_t* n, char* s) {
    return RanchuHwc2Device::from(dev)->getDisplayName(d, n, s);
}
int32_t fnGetDisplayRequests(hwc2_device_t*, hwc2_display_t d, int32_t* dispReq,
                             uint32_t* n, hwc2_layer_t*, int32_t*) {
    if (d != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
    if (dispReq) *dispReq = 0;
    if (n) *n = 0;
    return HWC2_ERROR_NONE;
}
int32_t fnGetDisplayType(hwc2_device_t* dev, hwc2_display_t d, int32_t* t) {
    return RanchuHwc2Device::from(dev)->getDisplayType(d, t);
}
int32_t fnGetDozeSupport(hwc2_device_t*, hwc2_display_t d, int32_t* outSupport) {
    if (d != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
    if (outSupport) *outSupport = 0;
    return HWC2_ERROR_NONE;
}
int32_t fnGetHdrCapabilities(hwc2_device_t*, hwc2_display_t d, uint32_t* outNumTypes,
                             int32_t*, float* maxLum, float* maxAvgLum, float* minLum) {
    if (d != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
    if (outNumTypes) *outNumTypes = 0;
    if (maxLum) *maxLum = 0.0f;
    if (maxAvgLum) *maxAvgLum = 0.0f;
    if (minLum) *minLum = 0.0f;
    return HWC2_ERROR_NONE;
}
uint32_t fnGetMaxVirtualDisplayCount(hwc2_device_t*) { return 0; }
int32_t fnGetReleaseFences(hwc2_device_t*, hwc2_display_t d, uint32_t* n,
                           hwc2_layer_t*, int32_t*) {
    if (d != kPrimaryDisplayId) return HWC2_ERROR_BAD_DISPLAY;
    if (n) *n = 0;
    return HWC2_ERROR_NONE;
}
int32_t fnPresentDisplay(hwc2_device_t* dev, hwc2_display_t d, int32_t* fence) {
    return RanchuHwc2Device::from(dev)->presentDisplay(d, fence);
}
int32_t fnRegisterCallback(hwc2_device_t* dev, int32_t desc, hwc2_callback_data_t data,
                           hwc2_function_pointer_t ptr) {
    return RanchuHwc2Device::from(dev)->registerCallback(desc, data, ptr);
}
int32_t fnSetActiveConfig(hwc2_device_t* dev, hwc2_display_t d, hwc2_config_t c) {
    return RanchuHwc2Device::from(dev)->setActiveConfig(d, c);
}
int32_t fnSetClientTarget(hwc2_device_t* dev, hwc2_display_t d, buffer_handle_t target,
                          int32_t fence, int32_t ds, hwc_region_t dmg) {
    return RanchuHwc2Device::from(dev)->setClientTarget(d, target, fence, ds, dmg);
}
int32_t fnSetColorMode(hwc2_device_t*, hwc2_display_t d, int32_t) {
    return (d == kPrimaryDisplayId) ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_DISPLAY;
}
int32_t fnSetColorTransform(hwc2_device_t*, hwc2_display_t d, const float*, int32_t) {
    return (d == kPrimaryDisplayId) ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_DISPLAY;
}
int32_t fnSetCursorPosition(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t, int32_t) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetLayerBlendMode(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetLayerBuffer(hwc2_device_t* dev, hwc2_display_t d, hwc2_layer_t l,
                         buffer_handle_t b, int32_t f) {
    return RanchuHwc2Device::from(dev)->setLayerBuffer(d, l, b, f);
}
int32_t fnSetLayerColor(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_color_t) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetLayerCompositionType(hwc2_device_t* dev, hwc2_display_t d, hwc2_layer_t l, int32_t t) {
    return RanchuHwc2Device::from(dev)->setLayerCompositionType(d, l, t);
}
int32_t fnSetLayerDataspace(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetLayerDisplayFrame(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_rect_t) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetLayerPlaneAlpha(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, float) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetLayerSidebandStream(hwc2_device_t*, hwc2_display_t, hwc2_layer_t,
                                 const native_handle_t*) {
    return HWC2_ERROR_UNSUPPORTED;
}
int32_t fnSetLayerSourceCrop(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_frect_t) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetLayerSurfaceDamage(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_region_t) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetLayerTransform(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetLayerVisibleRegion(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_region_t) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetLayerZOrder(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, uint32_t) {
    return HWC2_ERROR_NONE;
}
int32_t fnSetOutputBuffer(hwc2_device_t*, hwc2_display_t, buffer_handle_t, int32_t f) {
    if (f >= 0) close(f);
    return HWC2_ERROR_BAD_DISPLAY;
}
int32_t fnSetPowerMode(hwc2_device_t*, hwc2_display_t d, int32_t) {
    return (d == kPrimaryDisplayId) ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_DISPLAY;
}
int32_t fnSetVsyncEnabled(hwc2_device_t* dev, hwc2_display_t d, int32_t en) {
    return RanchuHwc2Device::from(dev)->setVsyncEnabled(d, en);
}
int32_t fnValidateDisplay(hwc2_device_t* dev, hwc2_display_t d, uint32_t* nt, uint32_t* nr) {
    return RanchuHwc2Device::from(dev)->validateDisplay(d, nt, nr);
}

void hwc2GetCapabilities(hwc2_device_t*, uint32_t* outCount, int32_t*) {
    if (outCount) *outCount = 0;
}

hwc2_function_pointer_t hwc2GetFunction(hwc2_device_t*, int32_t descriptor) {
    switch (descriptor) {
        case HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES:
            return reinterpret_cast<hwc2_function_pointer_t>(fnAcceptDisplayChanges);
        case HWC2_FUNCTION_CREATE_LAYER:
            return reinterpret_cast<hwc2_function_pointer_t>(fnCreateLayer);
        case HWC2_FUNCTION_CREATE_VIRTUAL_DISPLAY:
            return reinterpret_cast<hwc2_function_pointer_t>(fnCreateVirtualDisplay);
        case HWC2_FUNCTION_DESTROY_LAYER:
            return reinterpret_cast<hwc2_function_pointer_t>(fnDestroyLayer);
        case HWC2_FUNCTION_DESTROY_VIRTUAL_DISPLAY:
            return reinterpret_cast<hwc2_function_pointer_t>(fnDestroyVirtualDisplay);
        case HWC2_FUNCTION_DUMP:
            return reinterpret_cast<hwc2_function_pointer_t>(fnDump);
        case HWC2_FUNCTION_GET_ACTIVE_CONFIG:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetActiveConfig);
        case HWC2_FUNCTION_GET_CHANGED_COMPOSITION_TYPES:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetChangedCompositionTypes);
        case HWC2_FUNCTION_GET_CLIENT_TARGET_SUPPORT:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetClientTargetSupport);
        case HWC2_FUNCTION_GET_COLOR_MODES:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetColorModes);
        case HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetDisplayAttribute);
        case HWC2_FUNCTION_GET_DISPLAY_CONFIGS:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetDisplayConfigs);
        case HWC2_FUNCTION_GET_DISPLAY_NAME:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetDisplayName);
        case HWC2_FUNCTION_GET_DISPLAY_REQUESTS:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetDisplayRequests);
        case HWC2_FUNCTION_GET_DISPLAY_TYPE:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetDisplayType);
        case HWC2_FUNCTION_GET_DOZE_SUPPORT:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetDozeSupport);
        case HWC2_FUNCTION_GET_HDR_CAPABILITIES:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetHdrCapabilities);
        case HWC2_FUNCTION_GET_MAX_VIRTUAL_DISPLAY_COUNT:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetMaxVirtualDisplayCount);
        case HWC2_FUNCTION_GET_RELEASE_FENCES:
            return reinterpret_cast<hwc2_function_pointer_t>(fnGetReleaseFences);
        case HWC2_FUNCTION_PRESENT_DISPLAY:
            return reinterpret_cast<hwc2_function_pointer_t>(fnPresentDisplay);
        case HWC2_FUNCTION_REGISTER_CALLBACK:
            return reinterpret_cast<hwc2_function_pointer_t>(fnRegisterCallback);
        case HWC2_FUNCTION_SET_ACTIVE_CONFIG:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetActiveConfig);
        case HWC2_FUNCTION_SET_CLIENT_TARGET:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetClientTarget);
        case HWC2_FUNCTION_SET_COLOR_MODE:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetColorMode);
        case HWC2_FUNCTION_SET_COLOR_TRANSFORM:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetColorTransform);
        case HWC2_FUNCTION_SET_CURSOR_POSITION:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetCursorPosition);
        case HWC2_FUNCTION_SET_LAYER_BLEND_MODE:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerBlendMode);
        case HWC2_FUNCTION_SET_LAYER_BUFFER:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerBuffer);
        case HWC2_FUNCTION_SET_LAYER_COLOR:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerColor);
        case HWC2_FUNCTION_SET_LAYER_COMPOSITION_TYPE:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerCompositionType);
        case HWC2_FUNCTION_SET_LAYER_DATASPACE:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerDataspace);
        case HWC2_FUNCTION_SET_LAYER_DISPLAY_FRAME:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerDisplayFrame);
        case HWC2_FUNCTION_SET_LAYER_PLANE_ALPHA:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerPlaneAlpha);
        case HWC2_FUNCTION_SET_LAYER_SIDEBAND_STREAM:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerSidebandStream);
        case HWC2_FUNCTION_SET_LAYER_SOURCE_CROP:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerSourceCrop);
        case HWC2_FUNCTION_SET_LAYER_SURFACE_DAMAGE:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerSurfaceDamage);
        case HWC2_FUNCTION_SET_LAYER_TRANSFORM:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerTransform);
        case HWC2_FUNCTION_SET_LAYER_VISIBLE_REGION:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerVisibleRegion);
        case HWC2_FUNCTION_SET_LAYER_Z_ORDER:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetLayerZOrder);
        case HWC2_FUNCTION_SET_OUTPUT_BUFFER:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetOutputBuffer);
        case HWC2_FUNCTION_SET_POWER_MODE:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetPowerMode);
        case HWC2_FUNCTION_SET_VSYNC_ENABLED:
            return reinterpret_cast<hwc2_function_pointer_t>(fnSetVsyncEnabled);
        case HWC2_FUNCTION_VALIDATE_DISPLAY:
            return reinterpret_cast<hwc2_function_pointer_t>(fnValidateDisplay);
        default:
            return nullptr;
    }
}

int hwc2Close(hw_device_t* device) {
    delete RanchuHwc2Device::from(reinterpret_cast<hwc2_device_t*>(device));
    return 0;
}

int hwc2Open(const hw_module_t* module, const char* name, hw_device_t** device) {
    if (!name || strcmp(name, HWC_HARDWARE_COMPOSER) != 0) {
        return -EINVAL;
    }
    auto* dev = new RanchuHwc2Device();
    dev->base.common.tag = HARDWARE_DEVICE_TAG;
    dev->base.common.version = HWC_DEVICE_API_VERSION_2_0;
    dev->base.common.module = const_cast<hw_module_t*>(module);
    dev->base.common.close = hwc2Close;
    dev->base.getCapabilities = hwc2GetCapabilities;
    dev->base.getFunction = hwc2GetFunction;
    *device = &dev->base.common;
    return 0;
}

hw_module_methods_t gHwcModuleMethods = {
    .open = hwc2Open,
};

}  // namespace

extern "C" __attribute__((visibility("default")))
hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HWC_MODULE_API_VERSION_0_1,
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = HWC_HARDWARE_MODULE_ID,
    .name = "svbox-gfxstream HWC2",
    .author = "svbox-Android",
    .methods = &gHwcModuleMethods,
    .dso = nullptr,
    .reserved = {0},
};
