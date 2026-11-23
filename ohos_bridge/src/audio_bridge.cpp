/* SPDX-License-Identifier: MIT */
#include "ohos_bridge/audio_bridge.h"
#include "ohos_bridge/log.h"
#include "ohos_bridge/sched_qos.h"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr uint32_t kAudioMagic = 0x55415653u; /* "SVAU" */
constexpr size_t kRingCapacity = 262144u;     /* 256 KiB (~1.4s @ 44.1kHz stereo S16LE) */

/* HarmonyOS OHAudio ABI constants (matching <ohaudio/native_audiostream_base.h>) */
constexpr int32_t kAudioStreamSuccess = 0;
constexpr int32_t kAudioStreamTypeRenderer = 1;
constexpr int32_t kAudioSampleS16LE = 1;
constexpr int32_t kAudioEncodingRaw = 0;
constexpr int32_t kAudioUsageMusic = 1;
constexpr int32_t kAudioLatencyNormal = 0;
constexpr int32_t kAudioDataCallbackInvalid = -1;
constexpr int32_t kAudioDataCallbackValid = 0;

using WriteDataCb = int32_t (*)(void* renderer, void* userData, void* audioData, int32_t audioDataSize);

struct OhAudioApi {
    int32_t (*builderCreate)(void** builder, int32_t type) = nullptr;
    int32_t (*builderDestroy)(void* builder) = nullptr;
    int32_t (*setSamplingRate)(void* builder, int32_t rate) = nullptr;
    int32_t (*setChannelCount)(void* builder, int32_t channels) = nullptr;
    int32_t (*setSampleFormat)(void* builder, int32_t fmt) = nullptr;
    int32_t (*setEncodingType)(void* builder, int32_t enc) = nullptr;
    int32_t (*setRendererInfo)(void* builder, int32_t usage) = nullptr;
    int32_t (*setLatencyMode)(void* builder, int32_t mode) = nullptr;
    int32_t (*setWriteDataCb)(void* builder, WriteDataCb cb, void* userData) = nullptr;
    int32_t (*generateRenderer)(void* builder, void** renderer) = nullptr;
    int32_t (*rendererStart)(void* renderer) = nullptr;
    int32_t (*rendererPause)(void* renderer) = nullptr;
    int32_t (*rendererStop)(void* renderer) = nullptr;
    int32_t (*rendererRelease)(void* renderer) = nullptr;
};

OhAudioApi g_ohAudio;

struct AudioStreamHdr {
    uint32_t magic;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
};

bool RecvExact(int fd, void* dst, size_t n, const std::atomic<bool>& running) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while (got < n && running.load()) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 200);
        if (pr == 0) continue;
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r > 0) {
            got += static_cast<size_t>(r);
            continue;
        }
        if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return false;
    }
    return got == n;
}

bool HasNonSilentSamples(const uint8_t* data, size_t len) {
    const int16_t* s = reinterpret_cast<const int16_t*>(data);
    size_t n = len / 2u;
    for (size_t i = 0; i < n; i += 4) {
        if (s[i] > 8 || s[i] < -8) return true;
    }
    return false;
}

} // namespace

AudioBridge& AudioBridge::Instance() {
    static AudioBridge inst;
    return inst;
}

bool AudioBridge::EnsureOhAudioLoaded() {
    if (ohLoadTried_) {
        return backendAvailable_.load();
    }
    ohLoadTried_ = true;
    ohLib_ = dlopen("libohaudio.so", RTLD_NOW | RTLD_LOCAL);
    if (!ohLib_) {
        OB_LOG_WARN("SvboxAudio", "dlopen(libohaudio.so) unavailable: running in sink-only mode");
        backendAvailable_.store(false);
        return false;
    }
    auto loadSym = [this](const char* name) { return dlsym(ohLib_, name); };
    g_ohAudio.builderCreate = reinterpret_cast<decltype(g_ohAudio.builderCreate)>(loadSym("OH_AudioStreamBuilder_Create"));
    g_ohAudio.builderDestroy = reinterpret_cast<decltype(g_ohAudio.builderDestroy)>(loadSym("OH_AudioStreamBuilder_Destroy"));
    g_ohAudio.setSamplingRate = reinterpret_cast<decltype(g_ohAudio.setSamplingRate)>(loadSym("OH_AudioStreamBuilder_SetSamplingRate"));
    g_ohAudio.setChannelCount = reinterpret_cast<decltype(g_ohAudio.setChannelCount)>(loadSym("OH_AudioStreamBuilder_SetChannelCount"));
    g_ohAudio.setSampleFormat = reinterpret_cast<decltype(g_ohAudio.setSampleFormat)>(loadSym("OH_AudioStreamBuilder_SetSampleFormat"));
    g_ohAudio.setEncodingType = reinterpret_cast<decltype(g_ohAudio.setEncodingType)>(loadSym("OH_AudioStreamBuilder_SetEncodingType"));
    g_ohAudio.setRendererInfo = reinterpret_cast<decltype(g_ohAudio.setRendererInfo)>(loadSym("OH_AudioStreamBuilder_SetRendererInfo"));
    g_ohAudio.setLatencyMode = reinterpret_cast<decltype(g_ohAudio.setLatencyMode)>(loadSym("OH_AudioStreamBuilder_SetLatencyMode"));
    g_ohAudio.setWriteDataCb = reinterpret_cast<decltype(g_ohAudio.setWriteDataCb)>(loadSym("OH_AudioStreamBuilder_SetRendererWriteDataCallback"));
    g_ohAudio.generateRenderer = reinterpret_cast<decltype(g_ohAudio.generateRenderer)>(loadSym("OH_AudioStreamBuilder_GenerateRenderer"));
    g_ohAudio.rendererStart = reinterpret_cast<decltype(g_ohAudio.rendererStart)>(loadSym("OH_AudioRenderer_Start"));
    g_ohAudio.rendererPause = reinterpret_cast<decltype(g_ohAudio.rendererPause)>(loadSym("OH_AudioRenderer_Pause"));
    g_ohAudio.rendererStop = reinterpret_cast<decltype(g_ohAudio.rendererStop)>(loadSym("OH_AudioRenderer_Stop"));
    g_ohAudio.rendererRelease = reinterpret_cast<decltype(g_ohAudio.rendererRelease)>(loadSym("OH_AudioRenderer_Release"));

    bool ok = g_ohAudio.builderCreate && g_ohAudio.builderDestroy &&
              g_ohAudio.setSamplingRate && g_ohAudio.setChannelCount &&
              g_ohAudio.setSampleFormat && g_ohAudio.setEncodingType &&
              g_ohAudio.setRendererInfo && g_ohAudio.setLatencyMode &&
              g_ohAudio.setWriteDataCb && g_ohAudio.generateRenderer &&
              g_ohAudio.rendererStart && g_ohAudio.rendererPause &&
              g_ohAudio.rendererStop && g_ohAudio.rendererRelease;
    backendAvailable_.store(ok);
    if (!ok) {
        OB_LOG_WARN("SvboxAudio", "libohaudio.so missing required symbols");
    }
    return ok;
}

void AudioBridge::Start(const std::string& sockPath) {
    Stop();
    sockPath_ = sockPath;
    {
        std::lock_guard<std::mutex> lk(ringMu_);
        ring_.assign(kRingCapacity, 0);
        rHead_ = rTail_ = rUsed_ = 0;
        emptyCallbacks_ = 0;
    }
    bytesPlayed_.store(0);
    bytesReceived_.store(0);
    active_.store(false);
    running_.store(true);
    serverThread_ = std::thread(&AudioBridge::ServerLoop, this);
}

void AudioBridge::Stop() {
    running_.store(false);
    if (listenFd_ >= 0) {
        shutdown(listenFd_, SHUT_RDWR);
        close(listenFd_);
        listenFd_ = -1;
    }
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
    ReleaseRenderer();
    if (!sockPath_.empty()) {
        unlink(sockPath_.c_str());
    }
    active_.store(false);
}

void AudioBridge::PushPcm(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(ringMu_);
    if (ring_.empty()) return;
    const size_t cap = ring_.size();
    if (len >= cap) {
        data += (len - cap);
        len = cap;
        rHead_ = rTail_ = rUsed_ = 0;
    }
    if (rUsed_ + len > cap) {
        size_t drop = (rUsed_ + len) - cap;
        rHead_ = (rHead_ + drop) % cap;
        rUsed_ -= drop;
    }
    size_t first = std::min(len, cap - rTail_);
    std::memcpy(ring_.data() + rTail_, data, first);
    if (len > first) {
        std::memcpy(ring_.data(), data + first, len - first);
    }
    rTail_ = (rTail_ + len) % cap;
    rUsed_ += len;
    emptyCallbacks_ = 0;
}

size_t AudioBridge::PopPcm(uint8_t* dst, size_t len) {
    std::lock_guard<std::mutex> lk(ringMu_);
    if (ring_.empty() || rUsed_ == 0) {
        emptyCallbacks_++;
        return 0;
    }
    const size_t cap = ring_.size();
    size_t take = std::min(len, rUsed_);
    size_t first = std::min(take, cap - rHead_);
    std::memcpy(dst, ring_.data() + rHead_, first);
    if (take > first) {
        std::memcpy(dst + first, ring_.data(), take - first);
    }
    rHead_ = (rHead_ + take) % cap;
    rUsed_ -= take;
    emptyCallbacks_ = 0;
    return take;
}

int32_t AudioBridge::OnWriteData(void*, void* userData,
                                 void* audioData, int32_t audioDataSize) {
    auto* self = static_cast<AudioBridge*>(userData);
    if (!self || !audioData || audioDataSize <= 0) {
        return kAudioDataCallbackInvalid;
    }
    uint8_t* dst = static_cast<uint8_t*>(audioData);
    size_t need = static_cast<size_t>(audioDataSize);
    size_t got = self->PopPcm(dst, need);
    if (got < need) {
        std::memset(dst + got, 0, need - got);
    }
    if (self->muted_.load()) {
        std::memset(dst, 0, need);
    } else if (got > 0) {
        self->bytesPlayed_.fetch_add(got);
    }
    return kAudioDataCallbackValid;
}

bool AudioBridge::EnsureRenderer(uint32_t sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lk(ohMu_);
    if (!EnsureOhAudioLoaded()) {
        return false;
    }
    if (renderer_ && curRate_ == sampleRate && curChannels_ == channels) {
        if (!rendererStarted_) {
            if (g_ohAudio.rendererStart(renderer_) == kAudioStreamSuccess) {
                rendererStarted_ = true;
                active_.store(true);
            }
        }
        return true;
    }
    if (renderer_) {
        g_ohAudio.rendererStop(renderer_);
        g_ohAudio.rendererRelease(renderer_);
        renderer_ = nullptr;
        rendererStarted_ = false;
    }
    if (builder_) {
        g_ohAudio.builderDestroy(builder_);
        builder_ = nullptr;
    }

    if (g_ohAudio.builderCreate(&builder_, kAudioStreamTypeRenderer) != kAudioStreamSuccess || !builder_) {
        OB_LOG_WARN("SvboxAudio", "OH_AudioStreamBuilder_Create failed");
        return false;
    }
    g_ohAudio.setSamplingRate(builder_, static_cast<int32_t>(sampleRate));
    g_ohAudio.setChannelCount(builder_, static_cast<int32_t>(channels));
    g_ohAudio.setSampleFormat(builder_, kAudioSampleS16LE);
    g_ohAudio.setEncodingType(builder_, kAudioEncodingRaw);
    g_ohAudio.setRendererInfo(builder_, kAudioUsageMusic);
    g_ohAudio.setLatencyMode(builder_, kAudioLatencyNormal);
    g_ohAudio.setWriteDataCb(builder_, &AudioBridge::OnWriteData, this);

    int32_t rc = g_ohAudio.generateRenderer(builder_, &renderer_);
    if (rc != kAudioStreamSuccess || !renderer_) {
        OB_LOG_WARN("SvboxAudio", "OH_AudioStreamBuilder_GenerateRenderer failed: %d", rc);
        g_ohAudio.builderDestroy(builder_);
        builder_ = nullptr;
        return false;
    }

    curRate_ = sampleRate;
    curChannels_ = channels;
    if (g_ohAudio.rendererStart(renderer_) == kAudioStreamSuccess) {
        rendererStarted_ = true;
        active_.store(true);
        OB_LOG_INFO("SvboxAudio", "OHAudio renderer started: %u Hz, %u ch", sampleRate, channels);
    }
    return true;
}

void AudioBridge::ReleaseRenderer() {
    std::lock_guard<std::mutex> lk(ohMu_);
    if (renderer_ && g_ohAudio.rendererStop && g_ohAudio.rendererRelease) {
        g_ohAudio.rendererStop(renderer_);
        g_ohAudio.rendererRelease(renderer_);
        renderer_ = nullptr;
    }
    if (builder_ && g_ohAudio.builderDestroy) {
        g_ohAudio.builderDestroy(builder_);
        builder_ = nullptr;
    }
    rendererStarted_ = false;
    curRate_ = 0;
    curChannels_ = 0;
    active_.store(false);
}

void AudioBridge::ServerLoop() {
    SchedQos::SetCurrentThreadQoS(SchedQos::Level::UserInitiated, "audio-bridge");
    unlink(sockPath_.c_str());
    int sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sfd < 0) return;

    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, sockPath_.c_str(), sizeof(sa.sun_path) - 1);
    if (bind(sfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0 ||
        listen(sfd, 4) < 0) {
        close(sfd);
        return;
    }
    chmod(sockPath_.c_str(), 0666);
    listenFd_ = sfd;
    OB_LOG_INFO("SvboxAudio", "AudioBridge listening on %s", sockPath_.c_str());

    while (running_.load()) {
        struct pollfd pfd = { sfd, POLLIN, 0 };
        int r = poll(&pfd, 1, 200);
        if (r <= 0) {
            uint32_t empties = 0;
            {
                std::lock_guard<std::mutex> lk(ringMu_);
                empties = emptyCallbacks_;
            }
            if (empties >= 30) {
                std::lock_guard<std::mutex> lk(ohMu_);
                if (renderer_ && rendererStarted_ && g_ohAudio.rendererPause) {
                    g_ohAudio.rendererPause(renderer_);
                    rendererStarted_ = false;
                    active_.store(false);
                }
            }
            continue;
        }
        int cfd = accept4(sfd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) continue;
        HandleClient(cfd);
        close(cfd);
    }
}

void AudioBridge::HandleClient(int fd) {
    AudioStreamHdr hdr = {};
    if (!RecvExact(fd, &hdr, sizeof(hdr), running_) || hdr.magic != kAudioMagic) {
        return;
    }
    uint32_t rate = (hdr.sample_rate >= 8000 && hdr.sample_rate <= 96000) ? hdr.sample_rate : 44100u;
    uint32_t ch = (hdr.channels >= 1 && hdr.channels <= 2) ? hdr.channels : 2u;
    OB_LOG_INFO("SvboxAudio", "Audio stream connected: %u Hz %u ch", rate, ch);

    uint8_t buf[8192];
    int silentChunks = 0;
    while (running_.load()) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int r = poll(&pfd, 1, 250);
        if (r == 0) {
            silentChunks++;
            if (silentChunks >= 3) {
                std::lock_guard<std::mutex> lk(ohMu_);
                if (renderer_ && rendererStarted_ && g_ohAudio.rendererPause) {
                    g_ohAudio.rendererPause(renderer_);
                    rendererStarted_ = false;
                    active_.store(false);
                }
            }
            continue;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        bytesReceived_.fetch_add(static_cast<uint64_t>(n));

        if (HasNonSilentSamples(buf, static_cast<size_t>(n))) {
            silentChunks = 0;
            EnsureRenderer(rate, ch);
            PushPcm(buf, static_cast<size_t>(n));
        } else {
            if (active_.load()) {
                PushPcm(buf, static_cast<size_t>(n));
                silentChunks++;
                if (silentChunks > 30) {
                    std::lock_guard<std::mutex> lk(ohMu_);
                    if (renderer_ && rendererStarted_ && g_ohAudio.rendererPause) {
                        g_ohAudio.rendererPause(renderer_);
                        rendererStarted_ = false;
                        active_.store(false);
                    }
                }
            }
        }
    }
}
