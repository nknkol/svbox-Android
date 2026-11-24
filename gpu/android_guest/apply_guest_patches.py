#!/usr/bin/env python3
"""Idempotent patches to goldfish-opengl-r for Android 11 non-VIRTIO_GPU svbox guest build."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
GF = ROOT / "third_party" / "goldfish-opengl-r"


def patch_file(rel_path: str, old: str, new: str, replace_all: bool = False) -> None:
    p = GF / rel_path
    text = p.read_text(encoding="utf-8")
    if old not in text:
        if new in text:
            print(f"[ok] already patched: {rel_path}")
            return
        print(f"[ERR] expected pattern not found in {rel_path}", file=sys.stderr)
        sys.exit(1)
    updated = text.replace(old, new) if replace_all else text.replace(old, new, 1)
    p.write_text(updated, encoding="utf-8")
    print(f"[patched] {rel_path}")


def main() -> None:
    # 1. Keep local virtgpu_drm.h, guard xf86drm.h under VIRTIO_GPU in ResourceTracker.cpp.
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        '#ifdef HOST_BUILD\n#include "android/utils/tempfile.h"\n#else\n#include "virtgpu_drm.h"\n#include <xf86drm.h>\n#endif',
        '#ifdef HOST_BUILD\n#include "android/utils/tempfile.h"\n#else\n#include "virtgpu_drm.h"\n#ifdef VIRTIO_GPU\n#include <xf86drm.h>\n#else\nstatic inline int drmIoctl(int, unsigned long, void*) { errno = ENODEV; return -1; }\n#endif\n#endif',
    )

    # 2. Provide stub drmIoctl when VIRTIO_GPU is disabled in goldfish_address_space_android.impl.
    patch_file(
        "shared/GoldfishAddressSpace/include/goldfish_address_space_android.impl",
        "#ifdef VIRTIO_GPU\n#include <xf86drm.h>\n#endif",
        "#ifdef VIRTIO_GPU\n#include <xf86drm.h>\n#else\nstatic inline int drmIoctl(int, unsigned long, void*) { errno = ENODEV; return -1; }\n#endif",
    )

    # 3. Guard VirtioGpuPipeStream usage in ProcessPipe.cpp when VIRTIO_GPU is disabled.
    patch_file(
        "system/OpenglSystemCommon/ProcessPipe.cpp",
        "#if defined(HOST_BUILD) || !defined(GFXSTREAM)",
        "#if defined(HOST_BUILD) || !defined(GFXSTREAM) || !defined(VIRTIO_GPU)",
    )
    patch_file(
        "system/OpenglSystemCommon/ProcessPipe.cpp",
        "    } else {\n        delete sVirtioGpuPipeStream;\n        sVirtioGpuPipeStream = nullptr;\n    }",
        "    } else {\n#ifdef VIRTIO_GPU\n        delete sVirtioGpuPipeStream;\n        sVirtioGpuPipeStream = nullptr;\n#endif\n    }",
    )

    # 4. Support non-direct-mapped Vulkan host-visible memory over AF_UNIX pipe (no goldfish_address_space):
    #    - usingDirectMapping() matches host (hasDirectMem || hasVirtioGpuNext)
    #    - guest shadow buffer via demand-paged anonymous mmap
    #    - automatic coherent flush on vkUnmapMemory & vkQueueSubmit
    #    - fix VK_WHOLE_SIZE with non-zero offset in transform & VkEncoder
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """    struct VkDeviceMemory_Info {
        VkDeviceSize allocationSize = 0;
        VkDeviceSize mappedSize = 0;
        uint8_t* mappedPtr = nullptr;
        uint32_t memoryTypeIndex = 0;
        bool virtualHostVisibleBacking = false;
        bool directMapped = false;""",
        """    struct VkDeviceMemory_Info {
        VkDevice ownerDevice = VK_NULL_HANDLE;
        VkDeviceSize allocationSize = 0;
        VkDeviceSize mappedSize = 0;
        uint8_t* mappedPtr = nullptr;
        uint32_t memoryTypeIndex = 0;
        bool virtualHostVisibleBacking = false;
        bool directMapped = false;
        bool isMapped = false;
        VkDeviceSize mapOffset = 0;
        VkDeviceSize mapSize = 0;
        uint8_t* flushShadowPtr = nullptr;
        VkDeviceSize flushShadowSize = 0;
        std::vector<uint8_t> flushPageSynced;""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """    bool usingDirectMapping() const {
        return true;
    }""",
        """    bool usingDirectMapping() const {
        if (!mFeatureInfo) return false;
        return mFeatureInfo->hasDirectMem || mFeatureInfo->hasVirtioGpuNext;
    }""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """                if (offset) {
                    offset[i] = info.subAlloc.baseOffset + offset[i];
                }

                if (size) {
                    if (size[i] == VK_WHOLE_SIZE) {
                        size[i] = info.subAlloc.subMappedSize;
                    }
                }""",
        """                VkDeviceSize origOffset = offset ? offset[i] : 0;
                if (offset) {
                    offset[i] = info.subAlloc.baseOffset + origOffset;
                }

                if (size) {
                    if (size[i] == VK_WHOLE_SIZE) {
                        size[i] = (info.subAlloc.subMappedSize > origOffset)
                                      ? (info.subAlloc.subMappedSize - origOffset)
                                      : 0;
                    }
                }""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """#endif // VK_USE_PLATFORM_ANDROID_KHR
            }

            if (directMapResult != VK_SUCCESS) {""",
        """#endif // VK_USE_PLATFORM_ANDROID_KHR
            } else {
                // Non-direct-mapped pipe shadow memory (demand-paged anonymous mmap)
                void* shadow = mmap(nullptr, allocInfoForHost.allocationSize,
                                    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (shadow == MAP_FAILED || !shadow) {
                    ALOGE("%s: anonymous mmap(%llu) failed: errno=%d", __func__,
                          (unsigned long long)allocInfoForHost.allocationSize, errno);
                    directMapResult = VK_ERROR_OUT_OF_HOST_MEMORY;
                } else {
                    directMappedAddr = (uint64_t)(uintptr_t)shadow;
                    hostMemAlloc.rendernodeFd = -1;
                    hostMemAlloc.memoryAddr = directMappedAddr;
                    hostMemAlloc.memorySize = allocInfoForHost.allocationSize;
                    directMapResult = VK_SUCCESS;
                }
            }

            if (directMapResult != VK_SUCCESS) {""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        virtualMemInfo.allocationSize = virtualMemInfo.subAlloc.subAllocSize;
        virtualMemInfo.mappedSize = virtualMemInfo.subAlloc.subMappedSize;
        virtualMemInfo.mappedPtr = virtualMemInfo.subAlloc.mappedPtr;
        virtualMemInfo.memoryTypeIndex = finalAllocInfo.memoryTypeIndex;
        virtualMemInfo.directMapped = true;""",
        """        virtualMemInfo.ownerDevice = device;
        virtualMemInfo.allocationSize = virtualMemInfo.subAlloc.subAllocSize;
        virtualMemInfo.mappedSize = virtualMemInfo.subAlloc.subMappedSize;
        virtualMemInfo.mappedPtr = virtualMemInfo.subAlloc.mappedPtr;
        virtualMemInfo.memoryTypeIndex = finalAllocInfo.memoryTypeIndex;
        virtualMemInfo.directMapped = true;""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return;
        auto& info = it->second;
        uint64_t memoryObjectId = (uint64_t)(void*)memory;""",
        """        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return;
        auto& info = it->second;
        info.isMapped = false;
        if (info.flushShadowPtr && info.flushShadowSize > 0) {
            munmap(info.flushShadowPtr, info.flushShadowSize);
            info.flushShadowPtr = nullptr;
            info.flushShadowSize = 0;
            info.flushPageSynced.clear();
        }
        uint64_t memoryObjectId = (uint64_t)(void*)memory;""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        *ppData = info.mappedPtr + offset;

        return host_result;
    }

    void on_vkUnmapMemory(
        void*,
        VkDevice,
        VkDeviceMemory) {
        // no-op
    }""",
        """        info.isMapped = true;
        info.mapOffset = offset;
        info.mapSize = (size == VK_WHOLE_SIZE)
                           ? ((info.mappedSize > offset) ? (info.mappedSize - offset) : 0)
                           : size;

        *ppData = info.mappedPtr + offset;

        return host_result;
    }

    void collectCoherentDirtyRangesLocked(
        VkDeviceMemory memory,
        VkDeviceMemory_Info& info,
        VkDeviceSize reqOffset,
        VkDeviceSize reqSize,
        std::vector<VkMappedMemoryRange>& out) {
        if (!info.mappedPtr || info.mappedSize == 0) return;
        VkDeviceSize startOff = reqOffset < info.mappedSize ? reqOffset : info.mappedSize;
        VkDeviceSize endOff = (reqSize == VK_WHOLE_SIZE || reqSize > info.mappedSize - startOff)
                                  ? info.mappedSize
                                  : (startOff + reqSize);
        if (endOff <= startOff) return;

        constexpr VkDeviceSize kPage = 4096;
        VkDeviceSize scanStart = startOff & ~(kPage - 1);
        VkDeviceSize scanEnd = (endOff + kPage - 1) & ~(kPage - 1);
        if (scanEnd > info.mappedSize) scanEnd = info.mappedSize;

        if (!info.flushShadowPtr) {
            void* shadow = mmap(nullptr, info.mappedSize, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (shadow == MAP_FAILED || !shadow) {
                VkMappedMemoryRange r{};
                r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                r.memory = memory;
                r.offset = scanStart;
                r.size = scanEnd - scanStart;
                out.push_back(r);
                return;
            }
            info.flushShadowPtr = static_cast<uint8_t*>(shadow);
            info.flushShadowSize = info.mappedSize;
            size_t numPages = static_cast<size_t>((info.mappedSize + kPage - 1) / kPage);
            info.flushPageSynced.assign((numPages + 7) / 8, 0);
        }

        bool inDirty = false;
        VkDeviceSize dirtyStart = 0;
        VkDeviceSize dirtyEnd = 0;
        for (VkDeviceSize off = scanStart; off < scanEnd; off += kPage) {
            VkDeviceSize chunk = (scanEnd - off < kPage) ? (scanEnd - off) : kPage;
            size_t pageIdx = static_cast<size_t>(off / kPage);
            uint8_t& byteRef = info.flushPageSynced[pageIdx >> 3];
            uint8_t bitMask = static_cast<uint8_t>(1u << (pageIdx & 7));
            bool synced = (byteRef & bitMask) != 0;
            bool dirty = !synced ||
                         (memcmp(info.mappedPtr + off, info.flushShadowPtr + off, chunk) != 0);
            if (dirty) {
                memcpy(info.flushShadowPtr + off, info.mappedPtr + off, chunk);
                byteRef |= bitMask;
                if (!inDirty) {
                    inDirty = true;
                    dirtyStart = off;
                }
                dirtyEnd = off + chunk;
            } else if (inDirty) {
                VkMappedMemoryRange r{};
                r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                r.memory = memory;
                r.offset = dirtyStart;
                r.size = dirtyEnd - dirtyStart;
                out.push_back(r);
                inDirty = false;
            }
        }
        if (inDirty) {
            VkMappedMemoryRange r{};
            r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            r.memory = memory;
            r.offset = dirtyStart;
            r.size = dirtyEnd - dirtyStart;
            out.push_back(r);
        }
    }

    void on_vkUnmapMemory(
        void* context,
        VkDevice device,
        VkDeviceMemory memory) {
        std::vector<VkMappedMemoryRange> unmapRanges;
        {
            AutoLock<RecursiveLock> lock(mLock);
            auto it = info_VkDeviceMemory.find(memory);
            if (it != info_VkDeviceMemory.end()) {
                auto& info = it->second;
                if (!usingDirectMapping() && info.isMapped && info.directMapped &&
                    info.mappedPtr && info.mappedSize > 0) {
                    const auto& props = mHostVisibleMemoryVirtInfo.guestMemoryProperties;
                    if (info.memoryTypeIndex < props.memoryTypeCount &&
                        (props.memoryTypes[info.memoryTypeIndex].propertyFlags &
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                        collectCoherentDirtyRangesLocked(
                            memory, info, info.mapOffset, info.mapSize, unmapRanges);
                    }
                }
                info.isMapped = false;
            }
        }
        if (!unmapRanges.empty() && context) {
            VkEncoder* enc = (VkEncoder*)context;
            enc->vkFlushMappedMemoryRanges(
                device, static_cast<uint32_t>(unmapRanges.size()),
                unmapRanges.data(), true /* do lock */);
        }
    }""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """    VkResult on_vkQueueSubmit(
        void* context, VkResult input_result,
        VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
        AEMU_SCOPED_TRACE("on_vkQueueSubmit");

        flushStagingStreams(context, queue, submitCount, pSubmits);""",
        """    VkResult on_vkQueueSubmit(
        void* context, VkResult input_result,
        VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
        AEMU_SCOPED_TRACE("on_vkQueueSubmit");

        if (!usingDirectMapping() && context) {
            VkDevice qDev = VK_NULL_HANDLE;
            std::vector<VkMappedMemoryRange> coherentRanges;
            {
                AutoLock<RecursiveLock> lock(mLock);
                auto qIt = info_VkQueue.find(queue);
                if (qIt != info_VkQueue.end()) {
                    qDev = qIt->second.device;
                    const auto& props = mHostVisibleMemoryVirtInfo.guestMemoryProperties;
                    for (auto& kv : info_VkDeviceMemory) {
                        auto& info = kv.second;
                        if (info.ownerDevice == qDev && info.isMapped && info.directMapped &&
                            info.mappedPtr && info.mappedSize > 0 &&
                            info.memoryTypeIndex < props.memoryTypeCount &&
                            (props.memoryTypes[info.memoryTypeIndex].propertyFlags &
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                            collectCoherentDirtyRangesLocked(
                                kv.first, info, info.mapOffset, info.mapSize, coherentRanges);
                        }
                    }
                }
            }
            if (qDev != VK_NULL_HANDLE && !coherentRanges.empty()) {
                VkEncoder* enc = (VkEncoder*)context;
                enc->vkFlushMappedMemoryRanges(
                    qDev, static_cast<uint32_t>(coherentRanges.size()),
                    coherentRanges.data(), true /* do lock */);
            }
        }

        flushStagingStreams(context, queue, submitCount, pSubmits);""",
    )

    patch_file(
        "system/vulkan_enc/HostVisibleMemoryVirtualization.cpp",
        """    if (toDestroy->initResult != VK_SUCCESS) return;
    if (!toDestroy->initialized) return;""",
        """    if (toDestroy->rendernodeFd < 0 && toDestroy->memoryAddr && toDestroy->memorySize) {
        munmap((void*)(uintptr_t)toDestroy->memoryAddr, toDestroy->memorySize);
        toDestroy->memoryAddr = 0;
        toDestroy->memorySize = 0;
    }

    if (toDestroy->initResult != VK_SUCCESS) return;
    if (!toDestroy->initialized) return;""",
    )

    # Fix VK_WHOLE_SIZE with non-zero offset in VkEncoder Flush/Invalidate
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        "            auto actualSize = size == VK_WHOLE_SIZE ? sResourceTracker->getMappedSize(memory) : size;",
        "            auto actualSize = size == VK_WHOLE_SIZE ? (sResourceTracker->getMappedSize(memory) - offset) : size;",
        replace_all=True,
    )

    # 5. Also record ownerDevice in setDeviceMemoryInfo, cache /dev/goldfish_sync open failure,
    #    and synchronously drain host GPU when exporting SYNC_FD without kernel goldfish_sync.
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        auto& info = info_VkDeviceMemory[memory];

        info.allocationSize = allocationSize;""",
        """        auto& info = info_VkDeviceMemory[memory];

        info.ownerDevice = device;
        info.allocationSize = allocationSize;""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """    void ensureSyncDeviceFd() {
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (mSyncDeviceFd >= 0)
            return;
        mSyncDeviceFd = goldfish_sync_open();
        if (mSyncDeviceFd >= 0) {
            ALOGD("%s: created sync device for current Vulkan process: %d\\n", __func__, mSyncDeviceFd);
        } else {
            ALOGD("%s: failed to create sync device for current Vulkan process\\n", __func__);
        }
#endif
    }""",
        """    void ensureSyncDeviceFd() {
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        static bool sSyncTried = false;
        if (mSyncDeviceFd >= 0 || sSyncTried)
            return;
        sSyncTried = true;
        mSyncDeviceFd = goldfish_sync_open();
        if (mSyncDeviceFd >= 0) {
            ALOGD("%s: created sync device for current Vulkan process: %d\\n", __func__, mSyncDeviceFd);
        } else {
            ALOGD("%s: no /dev/goldfish_sync; using synchronous GPU wait fallback\\n", __func__);
        }
#endif
    }""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        if (getSyncFd) {
            AutoLock<RecursiveLock> lock(mLock);
            auto it = info_VkSemaphore.find(pGetFdInfo->semaphore);
            if (it == info_VkSemaphore.end()) return VK_ERROR_OUT_OF_HOST_MEMORY;
            auto& semInfo = it->second;
            *pFd = dup(semInfo.syncFd);
            return VK_SUCCESS;
        } else {""",
        """        if (getSyncFd) {
            int syncFd = -1;
            {
                AutoLock<RecursiveLock> lock(mLock);
                auto it = info_VkSemaphore.find(pGetFdInfo->semaphore);
                if (it == info_VkSemaphore.end()) return VK_ERROR_OUT_OF_HOST_MEMORY;
                syncFd = it->second.syncFd;
            }
            if (syncFd >= 0) {
                *pFd = dup(syncFd);
            } else {
                if (enc) {
                    enc->vkDeviceWaitIdle(device, true /* do lock */);
                }
                *pFd = -1;
            }
            return VK_SUCCESS;
        } else {""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """            } else {
                goldfish_sync_queue_work(
                    mSyncDeviceFd,
                    get_host_u64_VkFence(pGetFdInfo->fence) /* the handle */,
                    GOLDFISH_SYNC_VULKAN_SEMAPHORE_SYNC /* thread handle (doubling as type field) */,
                    pFd);
            }

            // relinquish ownership
            info.syncFd = -1;
            ALOGV("%s: got fd: %d\\n", __func__, *pFd);
            return VK_SUCCESS;""",
        """            } else if (mSyncDeviceFd >= 0) {
                goldfish_sync_queue_work(
                    mSyncDeviceFd,
                    get_host_u64_VkFence(pGetFdInfo->fence) /* the handle */,
                    GOLDFISH_SYNC_VULKAN_SEMAPHORE_SYNC /* thread handle (doubling as type field) */,
                    pFd);
            } else {
                info.syncFd = -1;
                lock.unlock();
                if (currentFenceStatus == VK_NOT_READY && enc) {
                    enc->vkWaitForFences(device, 1, &pGetFdInfo->fence, VK_TRUE, UINT64_MAX, true);
                }
                *pFd = -1;
                return VK_SUCCESS;
            }

            // relinquish ownership
            info.syncFd = -1;
            ALOGV("%s: got fd: %d\\n", __func__, *pFd);
            return VK_SUCCESS;""",
    )


    # 6. Pass through VkMemoryDedicatedAllocateInfo for AHardwareBuffer import/export
    #    allocations so host VK_OHOS_external_memory (DEDICATED_ONLY_BIT on Maleoon 916)
    #    receives the target VkImage in vkAllocateMemory.
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        bool shouldPassThroughDedicatedAllocInfo =
            !exportAllocateInfoPtr && !importAhbInfoPtr &&
            !importBufferCollectionInfoPtr && !importBufferCollectionInfoPtrX &&
            !importVmoInfoPtr;""",
        """        bool shouldPassThroughDedicatedAllocInfo =
            !importBufferCollectionInfoPtr && !importBufferCollectionInfoPtrX &&
            !importVmoInfoPtr;""",
    )


if __name__ == "__main__":
    main()
