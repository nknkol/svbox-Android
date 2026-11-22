/* SPDX-License-Identifier: MIT */
#include "audio_bridge.h"
#include "sched_qos.h"

#include <algorithm>
#include <cstring>
#include <errno.h>
#include <hilog/log.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#undef LOG_TAG
#define LOG_TAG "SvboxAudio"

namespace {

constexpr uint32_t kAudioMagic = 0x55415653u; /* "SVAU" */
constexpr size_t kRingCapacity = 262144u;     /* 256 KiB (~1.4s @ 44.1kHz stereo S16LE) */

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

int32_t AudioBridge::OnWriteData(OH_AudioRendererStruct*, void* userData,
                                 void* audioData, int32_t audioDataSize) {
    auto* self = static_cast<AudioBridge*>(userData);
    if (!self || !audioData || audioDataSize <= 0) {
        return AUDIO_DATA_CALLBACK_RESULT_INVALID;
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
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

bool AudioBridge::EnsureRenderer(uint32_t sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lk(ohMu_);
    if (renderer_ && curRate_ == sampleRate && curChannels_ == channels) {
        if (!rendererStarted_) {
            if (OH_AudioRenderer_Start(renderer_) == AUDIOSTREAM_SUCCESS) {
                rendererStarted_ = true;
                active_.store(true);
            }
        }
        return true;
    }
    if (renderer_) {
        OH_AudioRenderer_Stop(renderer_);
        OH_AudioRenderer_Release(renderer_);
        renderer_ = nullptr;
        rendererStarted_ = false;
    }
    if (builder_) {
        OH_AudioStreamBuilder_Destroy(builder_);
        builder_ = nullptr;
    }

    if (OH_AudioStreamBuilder_Create(&builder_, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS || !builder_) {
        OH_LOG_WARN(LOG_APP, "OH_AudioStreamBuilder_Create failed");
        return false;
    }
    OH_AudioStreamBuilder_SetSamplingRate(builder_, static_cast<int32_t>(sampleRate));
    OH_AudioStreamBuilder_SetChannelCount(builder_, static_cast<int32_t>(channels));
    OH_AudioStreamBuilder_SetSampleFormat(builder_, AUDIOSTREAM_SAMPLE_S16LE);
    OH_AudioStreamBuilder_SetEncodingType(builder_, AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetRendererInfo(builder_, AUDIOSTREAM_USAGE_MUSIC);
    OH_AudioStreamBuilder_SetLatencyMode(builder_, AUDIOSTREAM_LATENCY_MODE_NORMAL);

    OH_AudioStreamBuilder_SetRendererWriteDataCallback(
        builder_,
        reinterpret_cast<OH_AudioRenderer_OnWriteDataCallback>(&AudioBridge::OnWriteData),
        this);

    OH_AudioStream_Result rc = OH_AudioStreamBuilder_GenerateRenderer(builder_, &renderer_);
    if (rc != AUDIOSTREAM_SUCCESS || !renderer_) {
        OH_LOG_WARN(LOG_APP, "OH_AudioStreamBuilder_GenerateRenderer failed: %{public}d", rc);
        OH_AudioStreamBuilder_Destroy(builder_);
        builder_ = nullptr;
        return false;
    }

    curRate_ = sampleRate;
    curChannels_ = channels;
    if (OH_AudioRenderer_Start(renderer_) == AUDIOSTREAM_SUCCESS) {
        rendererStarted_ = true;
        active_.store(true);
        OH_LOG_INFO(LOG_APP, "OHAudio renderer started: %{public}u Hz, %{public}u ch", sampleRate, channels);
    }
    return true;
}

void AudioBridge::ReleaseRenderer() {
    std::lock_guard<std::mutex> lk(ohMu_);
    if (renderer_) {
        OH_AudioRenderer_Stop(renderer_);
        OH_AudioRenderer_Release(renderer_);
        renderer_ = nullptr;
    }
    if (builder_) {
        OH_AudioStreamBuilder_Destroy(builder_);
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
    OH_LOG_INFO(LOG_APP, "AudioBridge listening on %{public}s", sockPath_.c_str());

    while (running_.load()) {
        struct pollfd pfd = { sfd, POLLIN, 0 };
        int r = poll(&pfd, 1, 200);
        if (r <= 0) {
            // Auto-pause renderer after prolonged silence (~600ms)
            uint32_t empties = 0;
            {
                std::lock_guard<std::mutex> lk(ringMu_);
                empties = emptyCallbacks_;
            }
            if (empties >= 30) {
                std::lock_guard<std::mutex> lk(ohMu_);
                if (renderer_ && rendererStarted_) {
                    OH_AudioRenderer_Pause(renderer_);
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
    OH_LOG_INFO(LOG_APP, "Audio stream connected: %{public}u Hz %{public}u ch", rate, ch);

    uint8_t buf[8192];
    int silentChunks = 0;
    while (running_.load()) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int r = poll(&pfd, 1, 250);
        if (r == 0) {
            silentChunks++;
            if (silentChunks >= 3) {
                std::lock_guard<std::mutex> lk(ohMu_);
                if (renderer_ && rendererStarted_) {
                    OH_AudioRenderer_Pause(renderer_);
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
                    if (renderer_ && rendererStarted_) {
                        OH_AudioRenderer_Pause(renderer_);
                        rendererStarted_ = false;
                        active_.store(false);
                    }
                }
            }
        }
    }
}
