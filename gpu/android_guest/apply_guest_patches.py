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
        struct BoundRange {
            VkDeviceSize offset = 0;
            VkDeviceSize size = 0;
            uint32_t usage = 0;
            bool isBuffer = true;
        };
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
        std::vector<uint8_t> flushPageSynced;
        std::unordered_map<uint64_t, BoundRange> boundObjects;""",
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
            std::vector<VkCommandBuffer> submittedCbs;
            for (uint32_t i = 0; i < submitCount; ++i) {
                for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; ++j) {
                    submittedCbs.push_back(pSubmits[i].pCommandBuffers[j]);
                }
            }
            VkDevice qDev = VK_NULL_HANDLE;
            std::vector<VkMappedMemoryRange> coherentRanges;
            if (!submittedCbs.empty()) {
                std::vector<CmdBufRef> cmdBufRefs;
                collectCmdBufRefsBottomUp(submittedCbs, cmdBufRefs);

                AutoLock<RecursiveLock> lock(mLock);
                auto qIt = info_VkQueue.find(queue);
                if (qIt != info_VkQueue.end()) {
                    qDev = qIt->second.device;
                    const auto& props = mHostVisibleMemoryVirtInfo.guestMemoryProperties;
                    std::unordered_map<VkDeviceMemory, std::vector<std::pair<VkDeviceSize, VkDeviceSize>>> memIntervals;
                    for (const auto& ref : cmdBufRefs) {
                        auto bIt = info_VkBuffer.find(ref.buffer);
                        if (bIt == info_VkBuffer.end()) continue;
                        const auto& bInfo = bIt->second;
                        if (bInfo.currentBacking == VK_NULL_HANDLE || bInfo.currentBackingSize == 0) continue;
                        VkDeviceSize subOff = (ref.offset < bInfo.currentBackingSize) ? ref.offset : bInfo.currentBackingSize;
                        VkDeviceSize avail = bInfo.currentBackingSize - subOff;
                        VkDeviceSize subLen = (ref.size == VK_WHOLE_SIZE || ref.size > avail) ? avail : ref.size;
                        if (subLen == 0) continue;
                        VkDeviceSize s = bInfo.currentBackingOffset + subOff;
                        VkDeviceSize e = s + subLen;
                        memIntervals[bInfo.currentBacking].emplace_back(s, e);
                    }
                    constexpr uint32_t kCmdOnlyMask =
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
                    constexpr VkDeviceSize kPage = 4096;
                    for (auto& kv : info_VkDeviceMemory) {
                        auto& info = kv.second;
                        if (info.ownerDevice == qDev && info.isMapped && info.directMapped &&
                            info.mappedPtr && info.mappedSize > 0 &&
                            info.memoryTypeIndex < props.memoryTypeCount &&
                            (props.memoryTypes[info.memoryTypeIndex].propertyFlags &
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                            auto& ivs = memIntervals[kv.first];
                            for (const auto& bkv : info.boundObjects) {
                                const auto& br = bkv.second;
                                if (!br.isBuffer || (br.usage & ~kCmdOnlyMask) != 0) {
                                    if (br.size > 0) {
                                        ivs.emplace_back(br.offset, br.offset + br.size);
                                    }
                                }
                            }
                            if (ivs.empty()) continue;
                            VkDeviceSize mapLo = info.mapOffset;
                            VkDeviceSize mapHi = info.mapOffset + info.mapSize;
                            if (mapHi > info.mappedSize) mapHi = info.mappedSize;
                            std::vector<std::pair<VkDeviceSize, VkDeviceSize>> clipped;
                            clipped.reserve(ivs.size());
                            for (const auto& iv : ivs) {
                                VkDeviceSize s = iv.first < mapLo ? mapLo : iv.first;
                                VkDeviceSize e = iv.second > mapHi ? mapHi : iv.second;
                                if (e <= s) continue;
                                s = s & ~(kPage - 1);
                                e = (e + kPage - 1) & ~(kPage - 1);
                                if (e > info.mappedSize) e = info.mappedSize;
                                clipped.emplace_back(s, e);
                            }
                            if (clipped.empty()) continue;
                            std::sort(clipped.begin(), clipped.end());
                            VkDeviceSize curS = clipped[0].first;
                            VkDeviceSize curE = clipped[0].second;
                            for (size_t idx = 1; idx < clipped.size(); ++idx) {
                                if (clipped[idx].first <= curE) {
                                    if (clipped[idx].second > curE) curE = clipped[idx].second;
                                } else {
                                    collectCoherentDirtyRangesLocked(
                                        kv.first, info, curS, curE - curS, coherentRanges);
                                    curS = clipped[idx].first;
                                    curE = clipped[idx].second;
                                }
                            }
                            collectCoherentDirtyRangesLocked(
                                kv.first, info, curS, curE - curS, coherentRanges);
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

    # Track live VkBuffer / VkImage memory bindings on VkDeviceMemory_Info and
    # command-buffer referenced VkBuffer sub-ranges so on_vkQueueSubmit only scans
    # active ranges instead of unallocated/untouched regions of large VMA blocks.
    patch_file(
        "system/vulkan_enc/ResourceTracker.h",
        """    VkResult on_vkResetCommandBuffer(
        void* context, VkResult input_result,
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags);

    VkResult on_vkCreateImageView(""",
        """    VkResult on_vkResetCommandBuffer(
        void* context, VkResult input_result,
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags);

    void recordCommandBufferBufferRef(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkDeviceSize size);

    VkResult on_vkCreateImageView(""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """    struct CommandBufferPendingDescriptorSets {
        std::unordered_set<VkDescriptorSet> sets;
    };""",
        """    struct CmdBufRef {
        VkBuffer buffer;
        VkDeviceSize offset;
        VkDeviceSize size;
    };

    struct CommandBufferPendingDescriptorSets {
        std::unordered_set<VkDescriptorSet> sets;
        std::vector<CmdBufRef> bufRefs;
    };

    void recordCommandBufferBufferRef(
        VkCommandBuffer commandBuffer,
        VkBuffer buffer,
        VkDeviceSize offset,
        VkDeviceSize size) {
        if (!commandBuffer || buffer == VK_NULL_HANDLE) return;
        struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
        if (!cb) return;
        if (!cb->userPtr) {
            cb->userPtr = new CommandBufferPendingDescriptorSets;
        }
        auto* pending = (CommandBufferPendingDescriptorSets*)cb->userPtr;
        pending->bufRefs.push_back({buffer, offset, size});
    }

    void collectCmdBufRefsBottomUp(
        const std::vector<VkCommandBuffer>& workingSet,
        std::vector<CmdBufRef>& outRefs) {
        if (workingSet.empty()) return;
        std::vector<VkCommandBuffer> nextLevel;
        for (auto commandBuffer : workingSet) {
            struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(commandBuffer);
            if (!cb) continue;
            forAllObjects(cb->subObjects, [&nextLevel](void* secondary) {
                nextLevel.push_back((VkCommandBuffer)secondary);
            });
        }
        collectCmdBufRefsBottomUp(nextLevel, outRefs);
        for (auto cmdbuf : workingSet) {
            struct goldfish_VkCommandBuffer* cb = as_goldfish_VkCommandBuffer(cmdbuf);
            if (!cb || !cb->userPtr) continue;
            auto* pending = (CommandBufferPendingDescriptorSets*)cb->userPtr;
            if (!pending->bufRefs.empty()) {
                outRefs.insert(outRefs.end(), pending->bufRefs.begin(), pending->bufRefs.end());
            }
        }
    }""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        if (alsoClearPendingDescriptorSets && cb->userPtr) {
            CommandBufferPendingDescriptorSets* pendingSets = (CommandBufferPendingDescriptorSets*)cb->userPtr;
            pendingSets->sets.clear();
        }""",
        """        if (alsoClearPendingDescriptorSets && cb->userPtr) {
            CommandBufferPendingDescriptorSets* pendingSets = (CommandBufferPendingDescriptorSets*)cb->userPtr;
            pendingSets->sets.clear();
            pendingSets->bufRefs.clear();
        }""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """    void on_vkDestroyBuffer(
        void* context,
        VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkDestroyBuffer(device, buffer, pAllocator, true /* do lock */);
    }""",
        """    void trackBindBufferMemoryLocked(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
        auto bIt = info_VkBuffer.find(buffer);
        auto mIt = info_VkDeviceMemory.find(memory);
        if (bIt == info_VkBuffer.end() || mIt == info_VkDeviceMemory.end()) return;
        auto& bInfo = bIt->second;
        if (bInfo.currentBacking != VK_NULL_HANDLE) {
            auto oldIt = info_VkDeviceMemory.find(bInfo.currentBacking);
            if (oldIt != info_VkDeviceMemory.end()) {
                oldIt->second.boundObjects.erase(reinterpret_cast<uint64_t>(buffer));
            }
        }
        VkDeviceSize bSize = bInfo.createInfo.size;
        bInfo.currentBacking = memory;
        bInfo.currentBackingOffset = memoryOffset;
        bInfo.currentBackingSize = bSize;
        mIt->second.boundObjects[reinterpret_cast<uint64_t>(buffer)] = {
            memoryOffset, bSize, bInfo.createInfo.usage, true};
    }

    void trackBindImageMemoryLocked(VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
        auto iIt = info_VkImage.find(image);
        auto mIt = info_VkDeviceMemory.find(memory);
        if (iIt == info_VkImage.end() || mIt == info_VkDeviceMemory.end()) return;
        auto& iInfo = iIt->second;
        if (iInfo.currentBacking != VK_NULL_HANDLE) {
            auto oldIt = info_VkDeviceMemory.find(iInfo.currentBacking);
            if (oldIt != info_VkDeviceMemory.end()) {
                oldIt->second.boundObjects.erase(reinterpret_cast<uint64_t>(image));
            }
        }
        VkDeviceSize iSize = iInfo.baseRequirementsKnown
                                 ? iInfo.baseRequirements.size
                                 : ((mIt->second.allocationSize > memoryOffset)
                                        ? (mIt->second.allocationSize - memoryOffset)
                                        : 0);
        iInfo.currentBacking = memory;
        iInfo.currentBackingOffset = memoryOffset;
        iInfo.currentBackingSize = iSize;
        mIt->second.boundObjects[reinterpret_cast<uint64_t>(image)] = {
            memoryOffset, iSize, 0xFFFFFFFFu, false};
    }

    void on_vkDestroyBuffer(
        void* context,
        VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
        {
            AutoLock<RecursiveLock> lock(mLock);
            auto bIt = info_VkBuffer.find(buffer);
            if (bIt != info_VkBuffer.end() && bIt->second.currentBacking != VK_NULL_HANDLE) {
                auto mIt = info_VkDeviceMemory.find(bIt->second.currentBacking);
                if (mIt != info_VkDeviceMemory.end()) {
                    mIt->second.boundObjects.erase(reinterpret_cast<uint64_t>(buffer));
                }
                bIt->second.currentBacking = VK_NULL_HANDLE;
            }
        }
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkDestroyBuffer(device, buffer, pAllocator, true /* do lock */);
    }""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """    VkResult on_vkBindBufferMemory(
        void *context, VkResult,
        VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
        VkEncoder *enc = (VkEncoder *)context;
        return enc->vkBindBufferMemory(
            device, buffer, memory, memoryOffset, true /* do lock */);
    }

    VkResult on_vkBindBufferMemory2(
        void *context, VkResult,
        VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
        VkEncoder *enc = (VkEncoder *)context;
        return enc->vkBindBufferMemory2(
            device, bindInfoCount, pBindInfos, true /* do lock */);
    }

    VkResult on_vkBindBufferMemory2KHR(
        void *context, VkResult,
        VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
        VkEncoder *enc = (VkEncoder *)context;
        return enc->vkBindBufferMemory2KHR(
            device, bindInfoCount, pBindInfos, true /* do lock */);
    }""",
        """    VkResult on_vkBindBufferMemory(
        void *context, VkResult,
        VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
        VkEncoder *enc = (VkEncoder *)context;
        VkResult res = enc->vkBindBufferMemory(
            device, buffer, memory, memoryOffset, true /* do lock */);
        if (res == VK_SUCCESS) {
            AutoLock<RecursiveLock> lock(mLock);
            trackBindBufferMemoryLocked(buffer, memory, memoryOffset);
        }
        return res;
    }

    VkResult on_vkBindBufferMemory2(
        void *context, VkResult,
        VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
        VkEncoder *enc = (VkEncoder *)context;
        VkResult res = enc->vkBindBufferMemory2(
            device, bindInfoCount, pBindInfos, true /* do lock */);
        if (res == VK_SUCCESS && pBindInfos) {
            AutoLock<RecursiveLock> lock(mLock);
            for (uint32_t i = 0; i < bindInfoCount; ++i) {
                trackBindBufferMemoryLocked(
                    pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
            }
        }
        return res;
    }

    VkResult on_vkBindBufferMemory2KHR(
        void *context, VkResult,
        VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
        VkEncoder *enc = (VkEncoder *)context;
        VkResult res = enc->vkBindBufferMemory2KHR(
            device, bindInfoCount, pBindInfos, true /* do lock */);
        if (res == VK_SUCCESS && pBindInfos) {
            AutoLock<RecursiveLock> lock(mLock);
            for (uint32_t i = 0; i < bindInfoCount; ++i) {
                trackBindBufferMemoryLocked(
                    pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
            }
        }
        return res;
    }""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """            imageInfo.pendingQsriSyncFds.clear();
        }
#endif

        VkEncoder* enc = (VkEncoder*)context;""",
        """            imageInfo.pendingQsriSyncFds.clear();
            if (imageInfo.currentBacking != VK_NULL_HANDLE) {
                auto mIt = info_VkDeviceMemory.find(imageInfo.currentBacking);
                if (mIt != info_VkDeviceMemory.end()) {
                    mIt->second.boundObjects.erase(reinterpret_cast<uint64_t>(image));
                }
                imageInfo.currentBacking = VK_NULL_HANDLE;
            }
        }
#endif

        VkEncoder* enc = (VkEncoder*)context;""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        return enc->vkBindImageMemory(device, image, memory, memoryOffset, true /* do lock */);
    }

    VkResult on_vkBindImageMemory2(
        void* context, VkResult,
        VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
        VkEncoder* enc = (VkEncoder*)context;
        // Do not forward calls with invalid handles to host.
        if (!pBindInfos ||
            info_VkDeviceMemory.find(pBindInfos->memory) ==
                info_VkDeviceMemory.end() ||
            info_VkImage.find(pBindInfos->image) == info_VkImage.end()) {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return enc->vkBindImageMemory2(device, bindingCount, pBindInfos, true /* do lock */);
    }

    VkResult on_vkBindImageMemory2KHR(
        void* context, VkResult,
        VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
        VkEncoder* enc = (VkEncoder*)context;
        // Do not forward calls with invalid handles to host.
        if (!pBindInfos ||
            info_VkDeviceMemory.find(pBindInfos->memory) ==
                info_VkDeviceMemory.end() ||
            info_VkImage.find(pBindInfos->image) == info_VkImage.end()) {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return enc->vkBindImageMemory2KHR(device, bindingCount, pBindInfos, true /* do lock */);
    }""",
        """        VkResult res = enc->vkBindImageMemory(device, image, memory, memoryOffset, true /* do lock */);
        if (res == VK_SUCCESS) {
            AutoLock<RecursiveLock> lock(mLock);
            trackBindImageMemoryLocked(image, memory, memoryOffset);
        }
        return res;
    }

    VkResult on_vkBindImageMemory2(
        void* context, VkResult,
        VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
        VkEncoder* enc = (VkEncoder*)context;
        // Do not forward calls with invalid handles to host.
        if (!pBindInfos ||
            info_VkDeviceMemory.find(pBindInfos->memory) ==
                info_VkDeviceMemory.end() ||
            info_VkImage.find(pBindInfos->image) == info_VkImage.end()) {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        VkResult res = enc->vkBindImageMemory2(device, bindingCount, pBindInfos, true /* do lock */);
        if (res == VK_SUCCESS) {
            AutoLock<RecursiveLock> lock(mLock);
            for (uint32_t i = 0; i < bindingCount; ++i) {
                trackBindImageMemoryLocked(
                    pBindInfos[i].image, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
            }
        }
        return res;
    }

    VkResult on_vkBindImageMemory2KHR(
        void* context, VkResult,
        VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos) {
        VkEncoder* enc = (VkEncoder*)context;
        // Do not forward calls with invalid handles to host.
        if (!pBindInfos ||
            info_VkDeviceMemory.find(pBindInfos->memory) ==
                info_VkDeviceMemory.end() ||
            info_VkImage.find(pBindInfos->image) == info_VkImage.end()) {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        VkResult res = enc->vkBindImageMemory2KHR(device, bindingCount, pBindInfos, true /* do lock */);
        if (res == VK_SUCCESS) {
            AutoLock<RecursiveLock> lock(mLock);
            for (uint32_t i = 0; i < bindingCount; ++i) {
                trackBindImageMemoryLocked(
                    pBindInfos[i].image, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
            }
        }
        return res;
    }""",
    )

    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """void ResourceTracker::deviceMemoryTransform_tohost(""",
        """void ResourceTracker::recordCommandBufferBufferRef(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkDeviceSize size) {
    mImpl->recordCommandBufferBufferRef(commandBuffer, buffer, offset, size);
}

void ResourceTracker::deviceMemoryTransform_tohost(""",
    )

    # Record command-buffer buffer references in VkEncoder.cpp for coherent range tracking
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    ENCODER_DEBUG_LOG("vkCmdBindIndexBuffer(commandBuffer:%p, buffer:%p, offset:%ld)", commandBuffer, buffer, offset);""",
        """    ENCODER_DEBUG_LOG("vkCmdBindIndexBuffer(commandBuffer:%p, buffer:%p, offset:%ld)", commandBuffer, buffer, offset);
    if (sResourceTracker) sResourceTracker->recordCommandBufferBufferRef(commandBuffer, buffer, offset, VK_WHOLE_SIZE);""",
    )
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    ENCODER_DEBUG_LOG("vkCmdBindVertexBuffers(commandBuffer:%p, firstBinding:%d, bindingCount:%d, pBuffers:%p, pOffsets:%p)", commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);""",
        """    ENCODER_DEBUG_LOG("vkCmdBindVertexBuffers(commandBuffer:%p, firstBinding:%d, bindingCount:%d, pBuffers:%p, pOffsets:%p)", commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets);
    if (sResourceTracker && pBuffers) {
        for (uint32_t _bi = 0; _bi < bindingCount; ++_bi) {
            sResourceTracker->recordCommandBufferBufferRef(commandBuffer, pBuffers[_bi], pOffsets ? pOffsets[_bi] : 0, VK_WHOLE_SIZE);
        }
    }""",
    )
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    ENCODER_DEBUG_LOG("vkCmdCopyBuffer(commandBuffer:%p, srcBuffer:%p, dstBuffer:%p, regionCount:%d, pRegions:%p)", commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);""",
        """    ENCODER_DEBUG_LOG("vkCmdCopyBuffer(commandBuffer:%p, srcBuffer:%p, dstBuffer:%p, regionCount:%d, pRegions:%p)", commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
    if (sResourceTracker && pRegions) {
        for (uint32_t _ri = 0; _ri < regionCount; ++_ri) {
            sResourceTracker->recordCommandBufferBufferRef(commandBuffer, srcBuffer, pRegions[_ri].srcOffset, pRegions[_ri].size);
        }
    }""",
    )
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    ENCODER_DEBUG_LOG("vkCmdCopyBufferToImage(commandBuffer:%p, srcBuffer:%p, dstImage:%p, dstImageLayout:%d, regionCount:%d, pRegions:%p)", commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);""",
        """    ENCODER_DEBUG_LOG("vkCmdCopyBufferToImage(commandBuffer:%p, srcBuffer:%p, dstImage:%p, dstImageLayout:%d, regionCount:%d, pRegions:%p)", commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
    if (sResourceTracker && pRegions) {
        for (uint32_t _ri = 0; _ri < regionCount; ++_ri) {
            VkDeviceSize _rowT = pRegions[_ri].bufferRowLength ? pRegions[_ri].bufferRowLength : pRegions[_ri].imageExtent.width;
            VkDeviceSize _imgH = pRegions[_ri].bufferImageHeight ? pRegions[_ri].bufferImageHeight : pRegions[_ri].imageExtent.height;
            VkDeviceSize _dep = pRegions[_ri].imageExtent.depth ? pRegions[_ri].imageExtent.depth : 1;
            VkDeviceSize _lay = pRegions[_ri].imageSubresource.layerCount ? pRegions[_ri].imageSubresource.layerCount : 1;
            VkDeviceSize _maxB = _rowT * _imgH * _dep * _lay * 16ULL;
            if (_maxB < 4096ULL) _maxB = 4096ULL;
            sResourceTracker->recordCommandBufferBufferRef(commandBuffer, srcBuffer, pRegions[_ri].bufferOffset, _maxB);
        }
    }""",
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

    # Make OP_vkFlushMappedMemoryRanges async fire-and-forget (paired with host VkDecoder.cpp
    # on __OHOS__) to eliminate full IPC round-trip stalls before every vkQueueSubmit/vkUnmapMemory.
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    VkResult vkFlushMappedMemoryRanges_VkResult_return = (VkResult)0;
    stream->read(&vkFlushMappedMemoryRanges_VkResult_return, sizeof(VkResult));""",
        """    VkResult vkFlushMappedMemoryRanges_VkResult_return = VK_SUCCESS;
    stream->flush();""",
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
                // Flush ordered command stream so host submits the signaled work to Maleoon 916
                // immediately without stalling RenderThread on a full vkDeviceWaitIdle pipeline drain.
                if (enc) {
                    enc->flush();
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

    # 7. GLES eglSwapBuffers: avoid blocking RenderThread on synchronous glFinish (eglWaitClient)
    #    when native sync fd is absent; flushing the ordered command stream is sufficient because
    #    rcFlushWindowColorBufferAsync executes under host FrameBuffer lock before SurfaceFlinger
    #    composites the queued buffer on the next VSync.
    patch_file(
        "system/egl/egl.cpp",
        """    } else {
        // equivalent to glFinish if no native sync
        eglWaitClient();
    }""",
        """    } else {
        // Flush ordered GL stream without blocking RenderThread on synchronous glFinish round-trip.
        hostCon->flush();
    }""",
    )

    # 8. Make OP_vkBindBufferMemory, OP_vkResetFences, and OP_vkResetCommandPool
    #    async fire-and-forget (paired with host VkDecoder.cpp on __OHOS__) to avoid
    #    >1,100 unnecessary synchronous IPC round-trips every 2 seconds.
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    VkResult vkBindBufferMemory_VkResult_return = (VkResult)0;
    stream->read(&vkBindBufferMemory_VkResult_return, sizeof(VkResult));""",
        """    VkResult vkBindBufferMemory_VkResult_return = VK_SUCCESS;""",
    )
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    VkResult vkBindImageMemory_VkResult_return = (VkResult)0;
    stream->read(&vkBindImageMemory_VkResult_return, sizeof(VkResult));""",
        """    VkResult vkBindImageMemory_VkResult_return = VK_SUCCESS;""",
    )
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    VkResult vkResetFences_VkResult_return = (VkResult)0;
    stream->read(&vkResetFences_VkResult_return, sizeof(VkResult));""",
        """    VkResult vkResetFences_VkResult_return = VK_SUCCESS;""",
    )
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    VkResult vkResetCommandPool_VkResult_return = (VkResult)0;
    stream->read(&vkResetCommandPool_VkResult_return, sizeof(VkResult));""",
        """    VkResult vkResetCommandPool_VkResult_return = VK_SUCCESS;""",
    )

    # 9. Cache on_vkGetBufferMemoryRequirements2 / KHR from vkCreateBufferWithRequirementsGOOGLE
    #    (goldfish-opengl-r cached v1 but missed v2, causing ~900 redundant sync IPCs / 2s in Android 14 SkiaVK).
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """    void on_vkGetBufferMemoryRequirements2(
        void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements, true /* do lock */);
        transformBufferMemoryRequirements2ForGuest(
            pInfo->buffer, pMemoryRequirements);
    }

    void on_vkGetBufferMemoryRequirements2KHR(
        void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) {
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetBufferMemoryRequirements2KHR(device, pInfo, pMemoryRequirements, true /* do lock */);
        transformBufferMemoryRequirements2ForGuest(
            pInfo->buffer, pMemoryRequirements);
    }""",
        """    bool tryGetCachedBufferMemoryRequirements2(
        VkBuffer buffer, VkMemoryRequirements2* pMemoryRequirements) {
        AutoLock<RecursiveLock> lock(mLock);
        auto it = info_VkBuffer.find(buffer);
        if (it == info_VkBuffer.end() || !it->second.baseRequirementsKnown || it->second.external) {
            return false;
        }
        for (VkBaseOutStructure* ext = reinterpret_cast<VkBaseOutStructure*>(pMemoryRequirements->pNext);
             ext != nullptr; ext = ext->pNext) {
            if (ext->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
                return false;
            }
        }
        pMemoryRequirements->memoryRequirements = it->second.baseRequirements;
        for (VkBaseOutStructure* ext = reinterpret_cast<VkBaseOutStructure*>(pMemoryRequirements->pNext);
             ext != nullptr; ext = ext->pNext) {
            auto* ded = reinterpret_cast<VkMemoryDedicatedRequirements*>(ext);
            ded->prefersDedicatedAllocation = VK_FALSE;
            ded->requiresDedicatedAllocation = VK_FALSE;
        }
        return true;
    }

    void on_vkGetBufferMemoryRequirements2(
        void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) {
        if (pInfo && pMemoryRequirements &&
            tryGetCachedBufferMemoryRequirements2(pInfo->buffer, pMemoryRequirements)) {
            return;
        }
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements, true /* do lock */);
        transformBufferMemoryRequirements2ForGuest(
            pInfo->buffer, pMemoryRequirements);
    }

    void on_vkGetBufferMemoryRequirements2KHR(
        void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements) {
        if (pInfo && pMemoryRequirements &&
            tryGetCachedBufferMemoryRequirements2(pInfo->buffer, pMemoryRequirements)) {
            return;
        }
        VkEncoder* enc = (VkEncoder*)context;
        enc->vkGetBufferMemoryRequirements2KHR(device, pInfo, pMemoryRequirements, true /* do lock */);
        transformBufferMemoryRequirements2ForGuest(
            pInfo->buffer, pMemoryRequirements);
    }""",
    )

    # 10. Smart vkGetFenceStatus cache + post-submit poll throttle so SkiaVK's non-blocking
    #     fence polls don't drain the ordered host RenderThread stream immediately after submit.
    patch_file(
        "system/vulkan_enc/ResourceTracker.h",
        """    VkResult on_vkResetFences(
        void* context,
        VkResult input_result,
        VkDevice device,
        uint32_t fenceCount,
        const VkFence* pFences);""",
        """    VkResult on_vkResetFences(
        void* context,
        VkResult input_result,
        VkDevice device,
        uint32_t fenceCount,
        const VkFence* pFences);
    VkResult on_vkGetFenceStatus(
        void* context, VkResult input_result,
        VkDevice device, VkFence fence);""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """    struct VkFence_Info {
        VkDevice device;
        bool external = false;
        VkExportFenceCreateInfo exportFenceCreateInfo;
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        int syncFd = -1;
#endif
    };""",
        """    struct VkFence_Info {
        VkDevice device;
        bool external = false;
        VkExportFenceCreateInfo exportFenceCreateInfo;
        bool signaledCached = false;
        uint64_t lastSubmitNs = 0;
        VkQueue lastSubmitQueue = VK_NULL_HANDLE;
        uint32_t fastNotReadyCount = 0;
#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        int syncFd = -1;
#endif
    };""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        if (input_result != VK_SUCCESS) return input_result;

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (exportSyncFd) {""",
        """        if (input_result != VK_SUCCESS) return input_result;

        if (pCreateInfo && (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT)) {
            AutoLock<RecursiveLock> lock(mLock);
            auto it = info_VkFence.find(*pFence);
            if (it != info_VkFence.end()) {
                it->second.signaledCached = true;
            }
        }

#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (exportSyncFd) {""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        for (uint32_t i = 0; i < fenceCount; ++i) {
            VkFence fence = pFences[i];
            auto it = info_VkFence.find(fence);
            auto& info = it->second;
            if (!info.external) continue;""",
        """        for (uint32_t i = 0; i < fenceCount; ++i) {
            VkFence fence = pFences[i];
            auto it = info_VkFence.find(fence);
            if (it == info_VkFence.end()) continue;
            auto& info = it->second;
            info.signaledCached = false;
            info.lastSubmitNs = 0;
            info.lastSubmitQueue = VK_NULL_HANDLE;
            info.fastNotReadyCount = 0;
            if (!info.external) continue;""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        if (fencesExternal.empty()) {
            // No need for work pool, just wait with host driver.
            return enc->vkWaitForFences(
                device, fenceCount, pFences, waitAll, timeout, true /* do lock */);
        } else {""",
        """        if (fencesExternal.empty()) {
            bool allSignaled = (fenceCount > 0);
            {
                AutoLock<RecursiveLock> cLock(mLock);
                for (uint32_t i = 0; i < fenceCount; ++i) {
                    auto it = info_VkFence.find(pFences[i]);
                    if (it == info_VkFence.end() || !it->second.signaledCached) {
                        allSignaled = false;
                        break;
                    }
                }
            }
            if (allSignaled) return VK_SUCCESS;
            VkResult wRes = enc->vkWaitForFences(
                device, fenceCount, pFences, waitAll, timeout, true /* do lock */);
            if (wRes == VK_SUCCESS && (waitAll || fenceCount == 1)) {
                AutoLock<RecursiveLock> cLock(mLock);
                for (uint32_t i = 0; i < fenceCount; ++i) {
                    auto it = info_VkFence.find(pFences[i]);
                    if (it != info_VkFence.end()) it->second.signaledCached = true;
                }
            }
            return wRes;
        } else {""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """    VkResult on_vkCreateDescriptorPool(""",
        """    std::unordered_map<VkDevice, uint64_t> mDeviceLastSubmitNs;
    std::unordered_map<VkDevice, uint64_t> mDeviceNotReadySubmitNs;
    std::unordered_map<VkDevice, uint64_t> mDeviceNotReadyQueryNs;
    std::unordered_map<VkDevice, uint64_t> mDeviceLastFencePollNs;

    struct BufferReqCacheKey {
        VkDevice device;
        VkBufferCreateFlags flags;
        VkBufferUsageFlags usage;
        VkSharingMode sharingMode;
        bool operator==(const BufferReqCacheKey& o) const {
            return device == o.device && flags == o.flags &&
                   usage == o.usage && sharingMode == o.sharingMode;
        }
    };
    struct BufferReqCacheKeyHash {
        size_t operator()(const BufferReqCacheKey& k) const {
            size_t h = std::hash<uint64_t>{}(reinterpret_cast<uint64_t>(k.device));
            h ^= (static_cast<size_t>(k.flags) * 0x9e3779b97f4a7c15ULL);
            h ^= (static_cast<size_t>(k.usage) * 0xbf58476d1ce4e5b9ULL);
            h ^= (static_cast<size_t>(k.sharingMode) * 0x94d049bb133111ebULL);
            return h;
        }
    };
    struct CachedBufferReq {
        VkDeviceSize alignment = 256;
        uint32_t memoryTypeBits = 0;
    };
    std::unordered_map<BufferReqCacheKey, CachedBufferReq, BufferReqCacheKeyHash> mBufferReqCache;

    static uint64_t allocClientHandle() {
        static std::atomic<uint64_t> sSeq{1};
        uint64_t pidPart = (static_cast<uint64_t>(getpid()) & 0xFFFFFFULL) << 39;
        uint64_t seqPart = sSeq.fetch_add(1, std::memory_order_relaxed) & 0x7FFFFFFFFFULL;
        return (1ULL << 63) | pidPart | seqPart;
    }

    VkResult on_vkGetFenceStatus(
        void* context, VkResult,
        VkDevice device, VkFence fence) {
        struct timespec nowTs{};
        clock_gettime(CLOCK_MONOTONIC, &nowTs);
        uint64_t nowNs = (uint64_t)nowTs.tv_sec * 1000000000ULL + (uint64_t)nowTs.tv_nsec;
        uint64_t queriedFenceSubmitNs = 0;
        VkQueue queriedFenceQueue = VK_NULL_HANDLE;
        {
            AutoLock<RecursiveLock> lock(mLock);
            auto it = info_VkFence.find(fence);
            if (it != info_VkFence.end()) {
                auto& info = it->second;
                queriedFenceSubmitNs = info.lastSubmitNs;
                queriedFenceQueue = info.lastSubmitQueue;
                if (info.signaledCached) {
                    return VK_SUCCESS;
                }
                if (!info.external && info.lastSubmitNs > 0) {
                    uint64_t fenceAgeNs = (nowNs >= info.lastSubmitNs) ? (nowNs - info.lastSubmitNs) : 0;
                    // Safety cap: always poll host once a fence is >= 24ms old so no spin-wait
                    // or command-buffer recycling can ever stall.
                    if (fenceAgeNs < 24000000ULL) {
                        auto dIt = mDeviceLastSubmitNs.find(device);
                        bool recentDeviceSubmit =
                            (dIt != mDeviceLastSubmitNs.end() && nowNs >= dIt->second &&
                             (nowNs - dIt->second) < 3500000ULL);
                        bool recentFenceSubmit = (fenceAgeNs < 12000000ULL);
                        auto lpIt = mDeviceLastFencePollNs.find(device);
                        bool recentPoll =
                            (lpIt != mDeviceLastFencePollNs.end() && nowNs >= lpIt->second &&
                             (nowNs - lpIt->second) < 8000000ULL);
                        auto nrIt = mDeviceNotReadySubmitNs.find(device);
                        auto nqIt = mDeviceNotReadyQueryNs.find(device);
                        bool fifoBlocked =
                            (nrIt != mDeviceNotReadySubmitNs.end() && nqIt != mDeviceNotReadyQueryNs.end() &&
                             nrIt->second > 0 && info.lastSubmitNs >= nrIt->second &&
                             nowNs >= nqIt->second && (nowNs - nqIt->second) < 8000000ULL);
                        if (recentDeviceSubmit || recentFenceSubmit || recentPoll || fifoBlocked) {
                            return VK_NOT_READY;
                        }
                    }
                }
            }
        }
        VkEncoder* enc = (VkEncoder*)context;
        VkResult res = enc->vkGetFenceStatus(device, fence, true /* do lock */);
        {
            struct timespec t1{};
            clock_gettime(CLOCK_MONOTONIC, &t1);
            uint64_t t1Ns = (uint64_t)t1.tv_sec * 1000000000ULL + (uint64_t)t1.tv_nsec;
            AutoLock<RecursiveLock> lock(mLock);
            mDeviceLastFencePollNs[device] = t1Ns;
            auto it = info_VkFence.find(fence);
            if (it != info_VkFence.end()) {
                if (res == VK_SUCCESS) {
                    it->second.signaledCached = true;
                    // FIFO queue guarantee: all older fences submitted to the same queue are also signaled.
                    if (queriedFenceQueue != VK_NULL_HANDLE && queriedFenceSubmitNs > 0) {
                        for (auto& kv : info_VkFence) {
                            if (!kv.second.external && !kv.second.signaledCached &&
                                kv.second.lastSubmitQueue == queriedFenceQueue &&
                                kv.second.lastSubmitNs > 0 &&
                                kv.second.lastSubmitNs <= queriedFenceSubmitNs) {
                                kv.second.signaledCached = true;
                            }
                        }
                    }
                } else if (res == VK_NOT_READY) {
                    mDeviceNotReadyQueryNs[device] = t1Ns;
                    if (queriedFenceSubmitNs > 0) {
                        mDeviceNotReadySubmitNs[device] = queriedFenceSubmitNs;
                    }
                }
            }
        }
        return res;
    }

    VkResult on_vkCreateDescriptorPool(""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        if (fence != VK_NULL_HANDLE) {
            auto it = info_VkFence.find(fence);
            if (it != info_VkFence.end()) {
                const auto& info = it->second;
                if (info.syncFd >= 0) {
                    externalFenceFdToSignal = info.syncFd;
                }
            }
        }
#endif""",
        """#if defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(__linux__)
        {
            struct timespec subTs{};
            clock_gettime(CLOCK_MONOTONIC, &subTs);
            uint64_t subNs = (uint64_t)subTs.tv_sec * 1000000000ULL + (uint64_t)subTs.tv_nsec;
            auto qIt = info_VkQueue.find(queue);
            if (qIt != info_VkQueue.end()) {
                mDeviceLastSubmitNs[qIt->second.device] = subNs;
            }
            if (fence != VK_NULL_HANDLE) {
                auto it = info_VkFence.find(fence);
                if (it != info_VkFence.end()) {
                    auto& info = it->second;
                    info.signaledCached = false;
                    info.lastSubmitNs = subNs;
                    info.lastSubmitQueue = queue;
                    info.fastNotReadyCount = 0;
                    if (info.syncFd >= 0) {
                        externalFenceFdToSignal = info.syncFd;
                    }
                }
            }
        }
#endif""",
    )

    # 11. Async client-alias creation for vkCreateBufferWithRequirementsGOOGLE & vkCreateSemaphore
    #     (paired with host BoxedHandleManager client aliases & VkDecoder.cpp).
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        if (supportsCreateResourcesWithRequirements()) {
            res = enc->vkCreateBufferWithRequirementsGOOGLE(
                device, &localCreateInfo, pAllocator, pBuffer, &memReqs,
                true /* do lock */);
        } else {
            res = enc->vkCreateBuffer(device, &localCreateInfo, pAllocator,
                                      pBuffer, true /* do lock */);
        }""",
        """        if (supportsCreateResourcesWithRequirements()) {
            bool canAsyncBuf = !extBufCiPtr && localCreateInfo.pNext == nullptr &&
                               pCreateInfo->queueFamilyIndexCount == 0 && pAllocator == nullptr;
            bool cachedHit = false;
            BufferReqCacheKey reqKey{device, localCreateInfo.flags, localCreateInfo.usage, localCreateInfo.sharingMode};
            if (canAsyncBuf) {
                AutoLock<RecursiveLock> cLock(mLock);
                auto cIt = mBufferReqCache.find(reqKey);
                if (cIt != mBufferReqCache.end()) {
                    VkDeviceSize align = cIt->second.alignment ? cIt->second.alignment : 1;
                    memReqs.alignment = cIt->second.alignment;
                    memReqs.memoryTypeBits = cIt->second.memoryTypeBits;
                    memReqs.size = (localCreateInfo.size + align - 1) & ~(align - 1);
                    cachedHit = true;
                }
            }
            if (cachedHit) {
                *pBuffer = reinterpret_cast<VkBuffer>(allocClientHandle());
                res = enc->vkCreateBufferWithRequirementsGOOGLE(
                    device, &localCreateInfo, pAllocator, pBuffer, &memReqs,
                    true /* do lock */);
            } else {
                *pBuffer = VK_NULL_HANDLE;
                res = enc->vkCreateBufferWithRequirementsGOOGLE(
                    device, &localCreateInfo, pAllocator, pBuffer, &memReqs,
                    true /* do lock */);
                if (res == VK_SUCCESS && canAsyncBuf && memReqs.memoryTypeBits != 0) {
                    AutoLock<RecursiveLock> cLock(mLock);
                    mBufferReqCache[reqKey] = {memReqs.alignment, memReqs.memoryTypeBits};
                }
            }
        } else {
            *pBuffer = VK_NULL_HANDLE;
            res = enc->vkCreateBuffer(device, &localCreateInfo, pAllocator,
                                      pBuffer, true /* do lock */);
        }""",
    )
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    /* is handle, possibly out */;
    reservedmarshal_VkMemoryRequirements(stream, VK_STRUCTURE_TYPE_MAX_ENUM, (VkMemoryRequirements*)(pMemoryRequirements), streamPtrPtr);
    stream->setHandleMapping(sResourceTracker->createMapping());
    uint64_t cgen_var_3;
    stream->read((uint64_t*)&cgen_var_3, 8);
    stream->handleMapping()->mapHandles_u64_VkBuffer(&cgen_var_3, (VkBuffer*)pBuffer, 1);
    stream->unsetHandleMapping();
    unmarshal_VkMemoryRequirements(stream, VK_STRUCTURE_TYPE_MAX_ENUM, (VkMemoryRequirements*)(pMemoryRequirements));
    if (pMemoryRequirements)
    {
        transform_fromhost_VkMemoryRequirements(sResourceTracker, (VkMemoryRequirements*)(pMemoryRequirements));
    }
    VkResult vkCreateBufferWithRequirementsGOOGLE_VkResult_return = (VkResult)0;
    stream->read(&vkCreateBufferWithRequirementsGOOGLE_VkResult_return, sizeof(VkResult));""",
        """    /* is handle, possibly out */;
    reservedmarshal_VkMemoryRequirements(stream, VK_STRUCTURE_TYPE_MAX_ENUM, (VkMemoryRequirements*)(pMemoryRequirements), streamPtrPtr);
    VkResult vkCreateBufferWithRequirementsGOOGLE_VkResult_return = VK_SUCCESS;
    if ((cgen_var_2 & (1ULL << 63)) != 0)
    {
        stream->setHandleMapping(sResourceTracker->createMapping());
        uint64_t cgen_var_3 = cgen_var_2;
        stream->handleMapping()->mapHandles_u64_VkBuffer(&cgen_var_3, (VkBuffer*)pBuffer, 1);
        stream->unsetHandleMapping();
    }
    else
    {
        stream->setHandleMapping(sResourceTracker->createMapping());
        uint64_t cgen_var_3;
        stream->read((uint64_t*)&cgen_var_3, 8);
        stream->handleMapping()->mapHandles_u64_VkBuffer(&cgen_var_3, (VkBuffer*)pBuffer, 1);
        stream->unsetHandleMapping();
        unmarshal_VkMemoryRequirements(stream, VK_STRUCTURE_TYPE_MAX_ENUM, (VkMemoryRequirements*)(pMemoryRequirements));
        if (pMemoryRequirements)
        {
            transform_fromhost_VkMemoryRequirements(sResourceTracker, (VkMemoryRequirements*)(pMemoryRequirements));
        }
        vkCreateBufferWithRequirementsGOOGLE_VkResult_return = (VkResult)0;
        stream->read(&vkCreateBufferWithRequirementsGOOGLE_VkResult_return, sizeof(VkResult));
    }""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """        input_result = enc->vkCreateSemaphore(
            device, &finalCreateInfo, pAllocator, pSemaphore, true /* do lock */);""",
        """        if (finalCreateInfo.pNext == nullptr && pAllocator == nullptr) {
            *pSemaphore = reinterpret_cast<VkSemaphore>(allocClientHandle());
        } else {
            *pSemaphore = VK_NULL_HANDLE;
        }
        input_result = enc->vkCreateSemaphore(
            device, &finalCreateInfo, pAllocator, pSemaphore, true /* do lock */);""",
    )
    patch_file(
        "system/vulkan_enc/VkEncoder.cpp",
        """    /* is handle, possibly out */;
    stream->setHandleMapping(sResourceTracker->createMapping());
    uint64_t cgen_var_3;
    stream->read((uint64_t*)&cgen_var_3, 8);
    stream->handleMapping()->mapHandles_u64_VkSemaphore(&cgen_var_3, (VkSemaphore*)pSemaphore, 1);
    stream->unsetHandleMapping();
    VkResult vkCreateSemaphore_VkResult_return = (VkResult)0;
    stream->read(&vkCreateSemaphore_VkResult_return, sizeof(VkResult));""",
        """    /* is handle, possibly out */;
    VkResult vkCreateSemaphore_VkResult_return = VK_SUCCESS;
    if ((cgen_var_2 & (1ULL << 63)) != 0)
    {
        stream->setHandleMapping(sResourceTracker->createMapping());
        uint64_t cgen_var_3 = cgen_var_2;
        stream->handleMapping()->mapHandles_u64_VkSemaphore(&cgen_var_3, (VkSemaphore*)pSemaphore, 1);
        stream->unsetHandleMapping();
    }
    else
    {
        stream->setHandleMapping(sResourceTracker->createMapping());
        uint64_t cgen_var_3;
        stream->read((uint64_t*)&cgen_var_3, 8);
        stream->handleMapping()->mapHandles_u64_VkSemaphore(&cgen_var_3, (VkSemaphore*)pSemaphore, 1);
        stream->unsetHandleMapping();
        vkCreateSemaphore_VkResult_return = (VkResult)0;
        stream->read(&vkCreateSemaphore_VkResult_return, sizeof(VkResult));
    }""",
    )
    patch_file(
        "system/vulkan_enc/ResourceTracker.cpp",
        """VkResult ResourceTracker::on_vkResetFences(
    void* context,
    VkResult input_result,
    VkDevice device,
    uint32_t fenceCount,
    const VkFence* pFences) {
    return mImpl->on_vkResetFences(
        context, input_result, device, fenceCount, pFences);
}""",
        """VkResult ResourceTracker::on_vkResetFences(
    void* context,
    VkResult input_result,
    VkDevice device,
    uint32_t fenceCount,
    const VkFence* pFences) {
    return mImpl->on_vkResetFences(
        context, input_result, device, fenceCount, pFences);
}
VkResult ResourceTracker::on_vkGetFenceStatus(
    void* context, VkResult input_result,
    VkDevice device, VkFence fence) {
    return mImpl->on_vkGetFenceStatus(
        context, input_result, device, fence);
}""",
    )
    patch_file(
        "system/vulkan_enc/func_table.cpp",
        """static VkResult entry_vkGetFenceStatus(
    VkDevice device,
    VkFence fence)
{
    AEMU_SCOPED_TRACE("vkGetFenceStatus");
    auto vkEnc = ResourceTracker::getThreadLocalEncoder();
    VkResult vkGetFenceStatus_VkResult_return = (VkResult)0;
    vkGetFenceStatus_VkResult_return = vkEnc->vkGetFenceStatus(device, fence, true /* do lock */);
    return vkGetFenceStatus_VkResult_return;
}""",
        """static VkResult entry_vkGetFenceStatus(
    VkDevice device,
    VkFence fence)
{
    AEMU_SCOPED_TRACE("vkGetFenceStatus");
    auto vkEnc = ResourceTracker::getThreadLocalEncoder();
    auto resources = ResourceTracker::get();
    return resources->on_vkGetFenceStatus(vkEnc, VK_SUCCESS, device, fence);
}""",
    )

if __name__ == "__main__":
    main()
