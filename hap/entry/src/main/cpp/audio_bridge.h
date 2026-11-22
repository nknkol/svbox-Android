/* SPDX-License-Identifier: MIT */
/* Real HarmonyOS OHAudio PCM bridge: receives mixed PCM streams over
 * /ipc/audio.sock from audio.primary.svbox.so and plays them through
 * HarmonyOS PC's native libohaudio.so (OH_AudioRenderer) with automatic
 * silence pause for low-power compliance. */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct OH_AudioStreamBuilderStruct;
struct OH_AudioRendererStruct;

class AudioBridge {
public:
    static AudioBridge& Instance();

    void Start(const std::string& sockPath);
    void Stop();
    void SetMuted(bool muted) { muted_.store(muted); }
    bool IsMuted() const { return muted_.load(); }
    bool IsActive() const { return active_.load(); }
    uint64_t BytesPlayed() const { return bytesPlayed_.load(); }

private:
    AudioBridge() = default;

    void ServerLoop();
    void HandleClient(int fd);
    bool EnsureRenderer(uint32_t sampleRate, uint32_t channels);
    void ReleaseRenderer();
    void PushPcm(const uint8_t* data, size_t len);
    size_t PopPcm(uint8_t* dst, size_t len);

    static int32_t OnWriteData(OH_AudioRendererStruct* renderer, void* userData,
                               void* audioData, int32_t audioDataSize);

    std::atomic<bool> running_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> active_{false};
    std::atomic<uint64_t> bytesPlayed_{0};

    std::string sockPath_;
    int listenFd_ = -1;
    std::thread serverThread_;

    std::mutex ringMu_;
    std::vector<uint8_t> ring_;
    size_t rHead_ = 0;
    size_t rTail_ = 0;
    size_t rUsed_ = 0;
    uint32_t emptyCallbacks_ = 0;

    std::mutex ohMu_;
    OH_AudioStreamBuilderStruct* builder_ = nullptr;
    OH_AudioRendererStruct* renderer_ = nullptr;
    uint32_t curRate_ = 0;
    uint32_t curChannels_ = 0;
    bool rendererStarted_ = false;
};
