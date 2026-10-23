// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace BbGuestMemory {
inline constexpr vk::BufferUsageFlags ChunkUsage =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
    vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer;
vk::ExternalMemoryHandleTypeFlagBits ChunkHandleType();

struct Chunk {
    std::uint64_t phys = 0;
    std::uint64_t size = 0;
    vk::Buffer buffer;
    vk::DeviceMemory memory;
    std::uint32_t index = 0;
};

bool Usable(const Vulkan::Instance& instance);
bool PcModelGpu(const Vulkan::Instance& instance);
void Install(const Vulkan::Instance& instance);
const Chunk* Find(std::uint64_t phys);
}
