/* SPDX-License-Identifier: MIT */
/* HarmonyOS OHAudio PCM bridge: receives mixed PCM streams ("SVAU" header +
 * raw S16LE frames) over a Unix socket from any Linux/Android guest and plays
 * them through HarmonyOS PC's libohaudio.so (OH_AudioRenderer) with automatic
 * silence pause. Dynamically loads libohaudio.so so both HAP and standalone
 * CLI tools link cleanly. */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AudioBridge {
public:
    static AudioBridge& Instance();

    void Start(const std::string& sockPath);
    void Stop();
    void SetMuted(bool muted) { muted_.store(muted); }
    bool IsMuted() const { return muted_.load(); }
    bool IsActive() const { return active_.load(); }
    bool HasBackend() const { return backendAvailable_.load(); }
    uint64_t BytesPlayed() const { return bytesPlayed_.load(); }
    uint64_t BytesReceived() const { return bytesReceived_.load(); }

private:
    AudioBridge() = default;

    void ServerLoop();
    void HandleClient(int fd);
    bool EnsureOhAudioLoaded();
    bool EnsureRenderer(uint32_t sampleRate, uint32_t channels);
    void ReleaseRenderer();
    void PushPcm(const uint8_t* data, size_t len);
    size_t PopPcm(uint8_t* dst, size_t len);

    static int32_t OnWriteData(void* renderer, void* userData,
                               void* audioData, int32_t audioDataSize);

    std::atomic<bool> running_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> backendAvailable_{false};
    std::atomic<uint64_t> bytesPlayed_{0};
    std::atomic<uint64_t> bytesReceived_{0};

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
    void* ohLib_ = nullptr;
    bool ohLoadTried_ = false;
    void* builder_ = nullptr;
    void* renderer_ = nullptr;
    uint32_t curRate_ = 0;
    uint32_t curChannels_ = 0;
    bool rendererStarted_ = false;
};
