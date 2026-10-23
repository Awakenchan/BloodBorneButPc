// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "video_core/host_shaders/hdr_native_exposure_comp.h"
#include "video_core/host_shaders/hdr_native_gain_comp.h"
#include "video_core/host_shaders/hdr_native_luma_comp.h"
#include "video_core/host_shaders/hdr_native_snapshot_comp.h"
#include "video_core/host_shaders/hdr_native_stats_comp.h"

namespace {

#define CHECK(call)                                                                               \
    do {                                                                                          \
        const VkResult result_ = (call);                                                          \
        if (result_ != VK_SUCCESS) {                                                              \
            std::fprintf(stderr, "%s failed: %d\n", #call, int(result_));                         \
            std::exit(1);                                                                         \
        }                                                                                         \
    } while (0)

constexpr uint32_t W = 64, H = 32;
constexpr uint32_t LW = W / 2, LH = H / 2;
constexpr float Exposure = 0.25f;
constexpr float Knee = 0.5f;

uint16_t ToHalf(float value) {
    if (std::isnan(value)) {
        return 0x7e00;
    }
    if (std::isinf(value)) {
        return value > 0 ? 0x7c00 : 0xfc00;
    }
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000;
    const int exponent = int((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;
    if (exponent >= 31) {
        return uint16_t(sign | 0x7c00);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return uint16_t(sign);
        }
        mantissa |= 0x800000;
        const int shift = 14 - exponent;
        return uint16_t(sign | ((mantissa + (1u << (shift - 1))) >> shift));
    }
    const uint32_t rounded = (uint32_t(exponent) << 10 | mantissa >> 13) + ((mantissa >> 12) & 1);
    return uint16_t(sign | rounded);
}

float FromHalf(uint16_t v) {
    const int exponent = (v >> 10) & 31;
    const float mantissa = float(v & 1023);
    const float sign = v & 0x8000 ? -1.0f : 1.0f;
    if (exponent == 31) {
        return mantissa ? std::numeric_limits<float>::quiet_NaN()
                        : sign * std::numeric_limits<float>::infinity();
    }
    return sign * (exponent == 0 ? std::ldexp(mantissa, -24)
                                 : std::ldexp(1.0f + mantissa / 1024.0f, exponent - 15));
}

float Curve(float x) {
    return x <= 0.5f ? x : 0.5f + 0.5f * (1.0f - std::exp(-(x - 0.5f) / 0.5f));
}
uint8_t Encode(float linear) {
    return uint8_t(std::lround(std::pow(std::clamp(linear, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f));
}

struct Vk {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = 0;
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkPhysicalDeviceMemoryProperties memory{};

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
                return i;
            }
        }
        std::fprintf(stderr, "no memory type\n");
        std::exit(1);
    }
};

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkFormat format{};
    uint32_t width = 0, height = 0, bytes = 0;
};

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

Image MakeImage(const Vk& vk, VkFormat format, uint32_t width, uint32_t height, uint32_t bytes) {
    Image image{.format = format, .width = width, .height = height, .bytes = bytes};
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK(vkCreateImage(vk.device, &info, nullptr, &image.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk.device, image.image, &requirements);
    const VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex =
            vk.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    CHECK(vkAllocateMemory(vk.device, &alloc, nullptr, &image.memory));
    CHECK(vkBindImageMemory(vk.device, image.image, image.memory, 0));
    const VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    CHECK(vkCreateImageView(vk.device, &view, nullptr, &image.view));
    return image;
}

Buffer MakeBuffer(const Vk& vk, VkDeviceSize size, VkBufferUsageFlags usage, bool host = true) {
    Buffer buffer{.size = size};
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
    };
    CHECK(vkCreateBuffer(vk.device, &info, nullptr, &buffer.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk.device, buffer.buffer, &requirements);
    const VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = vk.MemoryType(requirements.memoryTypeBits,
                                         host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                              : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    CHECK(vkAllocateMemory(vk.device, &alloc, nullptr, &buffer.memory));
    CHECK(vkBindBufferMemory(vk.device, buffer.buffer, buffer.memory, 0));
    if (host) {
        CHECK(vkMapMemory(vk.device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped));
    }
    return buffer;
}

struct Pass {
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorSet set{};
    std::vector<VkDescriptorType> types;
};

Pass MakePass(const Vk& vk, VkDescriptorPool pool, std::vector<VkDescriptorType> types,
              uint32_t push_size, const uint32_t* code, size_t code_size) {
    Pass pass{.types = types};
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < types.size(); ++i) {
        bindings.push_back({i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    const VkDescriptorSetLayoutCreateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = uint32_t(bindings.size()),
        .pBindings = bindings.data(),
    };
    CHECK(vkCreateDescriptorSetLayout(vk.device, &set_info, nullptr, &pass.set_layout));
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    const VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &pass.set_layout,
        .pushConstantRangeCount = push_size ? 1u : 0u,
        .pPushConstantRanges = push_size ? &push : nullptr,
    };
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &pass.layout));
    const VkShaderModuleCreateInfo module_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code_size,
        .pCode = code,
    };
    VkShaderModule module;
    CHECK(vkCreateShaderModule(vk.device, &module_info, nullptr, &module));
    const VkComputePipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = pass.layout,
    };
    CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                   &pass.pipeline));
    vkDestroyShaderModule(vk.device, module, nullptr);
    const VkDescriptorSetAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &pass.set_layout,
    };
    CHECK(vkAllocateDescriptorSets(vk.device, &alloc, &pass.set));
    return pass;
}

struct Resource {
    const Image* image = nullptr;
    const Buffer* buffer = nullptr;
};

void Bind(const Vk& vk, const Pass& pass, std::vector<Resource> resources) {
    std::vector<VkDescriptorImageInfo> images(resources.size());
    std::vector<VkDescriptorBufferInfo> buffers(resources.size());
    std::vector<VkWriteDescriptorSet> writes;
    for (uint32_t i = 0; i < resources.size(); ++i) {
        VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass.set,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = pass.types[i],
        };
        if (resources[i].image) {
            images[i] = {VK_NULL_HANDLE, resources[i].image->view, VK_IMAGE_LAYOUT_GENERAL};
            write.pImageInfo = &images[i];
        } else {
            buffers[i] = {resources[i].buffer->buffer, 0, VK_WHOLE_SIZE};
            write.pBufferInfo = &buffers[i];
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(vk.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
}

void Barrier(VkCommandBuffer cmd) {
    const VkMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

void Dispatch(VkCommandBuffer cmd, const Pass& pass, const void* push, uint32_t push_size,
              uint32_t x, uint32_t y) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass.layout, 0, 1, &pass.set, 0,
                            nullptr);
    if (push_size) {
        vkCmdPushConstants(cmd, pass.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    }
    vkCmdDispatch(cmd, x, y, 1);
    Barrier(cmd);
}

void Begin(const Vk& vk) {
    CHECK(vkResetCommandBuffer(vk.cmd, 0));
    const VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkBeginCommandBuffer(vk.cmd, &begin));
}

void Submit(const Vk& vk) {
    CHECK(vkEndCommandBuffer(vk.cmd));
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &vk.cmd,
    };
    CHECK(vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(vk.queue));
}

void Upload(const Vk& vk, const Buffer& staging, const Image& image, const void* data) {
    std::memcpy(staging.mapped, data, size_t(image.width) * image.height * image.bytes);
    Begin(vk);
    const VkBufferImageCopy region{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {image.width, image.height, 1},
    };
    vkCmdCopyBufferToImage(vk.cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &region);
    Barrier(vk.cmd);
    Submit(vk);
}

template <typename T>
std::vector<T> Download(const Vk& vk, const Buffer& staging, const Image& image) {
    Begin(vk);
    const VkBufferImageCopy region{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {image.width, image.height, 1},
    };
    vkCmdCopyImageToBuffer(vk.cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1,
                           &region);
    Barrier(vk.cmd);
    Submit(vk);
    std::vector<T> out(size_t(image.width) * image.height * image.bytes / sizeof(T));
    std::memcpy(out.data(), staging.mapped, out.size() * sizeof(T));
    return out;
}

Vk Init() {
    Vk vk;
    const VkApplicationInfo app{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "hdr-native-shader-test",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo instance_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    CHECK(vkCreateInstance(&instance_info, nullptr, &vk.instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(vk.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vkEnumeratePhysicalDevices(vk.instance, &count, devices.data()));
    const char* wanted = std::getenv("BB_TEST_GPU");
    for (uint32_t i = 0; i < count && !vk.physical; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if ((wanted && uint32_t(std::atoi(wanted)) != i) ||
            properties.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }
        vk.physical = devices[i];
        std::printf("HDR native shaders: %s\n", properties.deviceName);
    }
    assert(vk.physical);
    vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family_properties(families);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, family_properties.data());
    while (!(family_properties[vk.family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        ++vk.family;
    }
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(vk.physical, &supported);
    assert(supported.shaderStorageImageExtendedFormats);
    const VkPhysicalDeviceFeatures features{.shaderStorageImageExtendedFormats = VK_TRUE};
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk.family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .pEnabledFeatures = &features,
    };
    CHECK(vkCreateDevice(vk.physical, &device_info, nullptr, &vk.device));
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk.family,
    };
    CHECK(vkCreateCommandPool(vk.device, &pool_info, nullptr, &vk.pool));
    const VkCommandBufferAllocateInfo cmd_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CHECK(vkAllocateCommandBuffers(vk.device, &cmd_info, &vk.cmd));
    return vk;
}

constexpr uint32_t CurveBins = 16, TilesX = 8, TilesY = 6, Tiles = TilesX * TilesY;
constexpr uint32_t PartialStride = 1 + CurveBins / 2;
constexpr float CurveLow = 0.5f, CurveHigh = 0.97f;
constexpr float RollOffStart = 0.7f;
struct ExposureState {
    float log_scale, valid, samples, frames;
    float curve[CurveBins][4];
    float tiles[Tiles][4];
};

struct ExposureParams {
    uint32_t count, reset;
    float min_samples, rate;
    uint32_t groups_x;
    float min_tile;
};
struct GainParams {
    float strength, white, knee, rolloff;
};

float RollOff(float x, float limit) {
    if (limit <= 1.0f) {
        return std::min(x, limit);
    }
    const float top = std::log2(limit), start = RollOffStart * top, lx = std::log2(x);
    if (lx <= start) {
        return x;
    }
    const float room = top - start;
    return std::exp2(start + room * (1.0f - std::exp(-(lx - start) / room)));
}

float Smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void Bench(const Vk& vk, const Pass& luma, const Pass& snapshot, const Pass& exposure,
           const Pass& gain, uint32_t width, uint32_t height) {
    const Image scene = MakeImage(vk, VK_FORMAT_R16G16B16A16_SFLOAT, width, height, 8);
    const Image luma_image =
        MakeImage(vk, VK_FORMAT_R16G16_SFLOAT, (width + 1) / 2, (height + 1) / 2, 4);
    const Image target = MakeImage(vk, VK_FORMAT_R8G8B8A8_UNORM, width, height, 4);
    const Image copy = MakeImage(vk, VK_FORMAT_R8G8B8A8_UNORM, width, height, 4);
    const Image gain_map = MakeImage(vk, VK_FORMAT_R16_SFLOAT, width, height, 2);
    const uint32_t groups_x = (width + 15) / 16, groups_y = (height + 15) / 16;
    const Buffer partials =
        MakeBuffer(vk, groups_x * groups_y * 16 * PartialStride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    const Buffer state = MakeBuffer(vk, sizeof(ExposureState),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    false);
    const Buffer staging = MakeBuffer(vk, VkDeviceSize(width) * height * 8,
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Begin(vk);
    vkCmdFillBuffer(vk.cmd, state.buffer, 0, VK_WHOLE_SIZE, 0);
    Barrier(vk.cmd);
    std::vector<VkImageMemoryBarrier> to_general;
    for (const Image* image : {&scene, &luma_image, &target, &copy, &gain_map}) {
        to_general.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image->image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});
    }
    vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                         uint32_t(to_general.size()), to_general.data());
    Submit(vk);
    uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    std::vector<uint16_t> scene_data(size_t(width) * height * 4);
    for (auto& v : scene_data) {
        v = ToHalf(float(next() % 4096) / 512.0f);
    }
    Upload(vk, staging, scene, scene_data.data());
    std::vector<uint8_t> frame_data(size_t(width) * height * 4);
    for (auto& v : frame_data) {
        v = uint8_t(next());
    }
    Upload(vk, staging, target, frame_data.data());
    Bind(vk, luma, {{&scene}, {&luma_image}});
    Bind(vk, snapshot, {{&target}, {&luma_image}, {nullptr, &partials}, {nullptr, &state},
                        {&copy}});
    Bind(vk, exposure, {{nullptr, &partials}, {nullptr, &state}});
    Bind(vk, gain, {{&target}, {&copy}, {&luma_image}, {nullptr, &state}, {&gain_map}});
    VkQueryPool queries;
    const VkQueryPoolCreateInfo query_info{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = 2,
    };
    CHECK(vkCreateQueryPool(vk.device, &query_info, nullptr, &queries));
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(vk.physical, &properties);
    const uint32_t reset = 0;
    const ExposureParams exposure_params{groups_x * groups_y, 0, 1024.0f, 0.1f, groups_x,
                                         float(std::max(64u, width * height / 200 / Tiles / 4))};
    const GainParams gain_params{0.5f, 5.0f, Knee, RollOffStart};
    std::vector<double> times;
    for (int run = 0; run < 120; ++run) {
        Begin(vk);
        vkCmdResetQueryPool(vk.cmd, queries, 0, 2);
        vkCmdWriteTimestamp(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        Dispatch(vk.cmd, luma, nullptr, 0, (luma_image.width + 7) / 8, (luma_image.height + 7) / 8);
        Dispatch(vk.cmd, snapshot, &reset, 4, groups_x, groups_y);
        Dispatch(vk.cmd, exposure, &exposure_params, sizeof(exposure_params), 1 + Tiles, 1);
        Dispatch(vk.cmd, gain, &gain_params, sizeof(gain_params), (width + 7) / 8,
                 (height + 7) / 8);
        vkCmdWriteTimestamp(vk.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        Submit(vk);
        std::array<uint64_t, 2> stamps{};
        CHECK(vkGetQueryPoolResults(vk.device, queries, 0, 2, sizeof(stamps), stamps.data(), 8,
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        if (run >= 20) {
            times.push_back(double(stamps[1] - stamps[0]) * properties.limits.timestampPeriod /
                            1e6);
        }
    }
    std::sort(times.begin(), times.end());
    std::printf("HDR native shaders: %ux%u, all passes: median %.3f ms, min %.3f ms, p90 %.3f ms\n",
                width, height, times[times.size() / 2], times.front(),
                times[times.size() * 9 / 10]);
    vkDestroyQueryPool(vk.device, queries, nullptr);
}

}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Vk vk = Init();
    VkDescriptorPool pool;
    const std::array<VkDescriptorPoolSize, 3> sizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
    }};
    const VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 8,
        .poolSizeCount = uint32_t(sizes.size()),
        .pPoolSizes = sizes.data(),
    };
    CHECK(vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &pool));
    constexpr auto Sampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    constexpr auto Storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    constexpr auto Ssbo = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    const Pass luma = MakePass(vk, pool, {Sampled, Storage}, 0, HDR_NATIVE_LUMA_COMP,
                               sizeof(HDR_NATIVE_LUMA_COMP));
    const Pass stats = MakePass(vk, pool, {Sampled, Sampled, Ssbo, Ssbo}, 4,
                                HDR_NATIVE_STATS_COMP, sizeof(HDR_NATIVE_STATS_COMP));
    const Pass snapshot = MakePass(vk, pool, {Sampled, Sampled, Ssbo, Ssbo, Storage}, 4,
                                   HDR_NATIVE_SNAPSHOT_COMP, sizeof(HDR_NATIVE_SNAPSHOT_COMP));
    const Pass exposure = MakePass(vk, pool, {Ssbo, Ssbo}, sizeof(ExposureParams), HDR_NATIVE_EXPOSURE_COMP,
                                   sizeof(HDR_NATIVE_EXPOSURE_COMP));
    const Pass gain = MakePass(vk, pool, {Sampled, Sampled, Sampled, Ssbo, Storage}, 16,
                               HDR_NATIVE_GAIN_COMP, sizeof(HDR_NATIVE_GAIN_COMP));
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
        const uint32_t width = argc > 3 ? uint32_t(std::atoi(argv[2])) : 1920;
        const uint32_t height = argc > 3 ? uint32_t(std::atoi(argv[3])) : 1080;
        Bench(vk, luma, snapshot, exposure, gain, width, height);
        vkDeviceWaitIdle(vk.device);
        return 0;
    }

    const Image scene = MakeImage(vk, VK_FORMAT_R16G16B16A16_SFLOAT, W, H, 8);
    const Image luma_image = MakeImage(vk, VK_FORMAT_R16G16_SFLOAT, LW, LH, 4);
    const Image target = MakeImage(vk, VK_FORMAT_R8G8B8A8_UNORM, W, H, 4);
    const Image copy = MakeImage(vk, VK_FORMAT_R8G8B8A8_UNORM, W, H, 4);
    const Image gain_map = MakeImage(vk, VK_FORMAT_R16_SFLOAT, W, H, 2);
    const uint32_t groups_x = (W + 15) / 16, groups_y = (H + 15) / 16;
    const Buffer partials = MakeBuffer(vk, groups_x * groups_y * 16 * PartialStride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const Buffer state = MakeBuffer(vk, sizeof(ExposureState), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const Buffer staging = MakeBuffer(vk, W * H * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::memset(state.mapped, 0, sizeof(ExposureState));
    Begin(vk);
    std::vector<VkImageMemoryBarrier> to_general;
    for (const Image* image : {&scene, &luma_image, &target, &copy, &gain_map}) {
        to_general.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image->image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});
    }
    vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                         uint32_t(to_general.size()), to_general.data());
    Submit(vk);

    enum Kind { Mid, Light, Bloom, Glow, Dark };
    std::vector<Kind> kinds(W * H);
    std::vector<float> scene_rgb(W * H * 3);
    std::vector<uint16_t> scene_half(W * H * 4);
    std::vector<uint8_t> before_ui(W * H * 4);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const uint32_t p = y * W + x;
            Kind kind = Mid;
            float level = (0.03f + 0.37f * float(x) / float(W - 1)) / Exposure;
            std::array<float, 3> tint{1.0f, 1.0f, 1.0f};
            if (x >= 40 && x < 48 && y >= 8 && y < 16) {
                kind = Light;
                level = 8.0f / Exposure;
                tint = {1.0f, 0.8f, 0.5f};
            } else if (x >= 38 && x < 50 && y >= 6 && y < 18) {
                kind = Bloom;
                level = 0.02f / Exposure;
            } else if (x >= 36 && x < 52 && y >= 4 && y < 20) {
                kind = Glow;
                level = 0.02f / Exposure;
            } else if (y >= 28) {
                kind = Dark;
                level = 0.001f;
            }
            kinds[p] = kind;
            for (int c = 0; c < 3; ++c) {
                scene_rgb[p * 3 + c] = level * tint[c];
                scene_half[p * 4 + c] = ToHalf(level * tint[c]);
                const float sdr = kind == Bloom  ? 0.8f
                                  : kind == Glow ? 0.3f
                                                 : Curve(Exposure * level * tint[c]);
                before_ui[p * 4 + c] = Encode(sdr);
            }
            scene_half[p * 4 + 3] = ToHalf(1.0f);
            before_ui[p * 4 + 3] = 255;
        }
    }
    std::vector<uint16_t> poisoned = scene_half;
    const uint32_t nan_pixel = 2 * W + 2, inf_pixel = 2 * W + 6;
    poisoned[nan_pixel * 4] = ToHalf(std::numeric_limits<float>::quiet_NaN());
    poisoned[inf_pixel * 4 + 1] = ToHalf(std::numeric_limits<float>::infinity());
    Upload(vk, staging, scene, poisoned.data());

    const auto reduce = [&] {
        Bind(vk, luma, {{&scene}, {&luma_image}});
        Begin(vk);
        Dispatch(vk.cmd, luma, nullptr, 0, (LW + 7) / 8, (LH + 7) / 8);
        Submit(vk);
    };
    reduce();
    const auto luma_out = Download<uint16_t>(vk, staging, luma_image);
    for (uint32_t y = 0; y < LH; ++y) {
        for (uint32_t x = 0; x < LW; ++x) {
            float peak = 0.0f;
            std::array<float, 3> sum{};
            for (uint32_t dy = 0; dy < 2; ++dy) {
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const uint32_t p = (y * 2 + dy) * W + x * 2 + dx;
                    for (int c = 0; c < 3; ++c) {
                        float v = FromHalf(poisoned[p * 4 + c]);
                        v = std::isnan(v) ? 0.0f : std::min(v, 65504.0f);
                        peak = std::max(peak, v);
                        sum[c] += v;
                    }
                }
            }
            const float average = std::max({sum[0], sum[1], sum[2]}) * 0.25f;
            const float r = FromHalf(luma_out[(y * LW + x) * 2]);
            const float g = FromHalf(luma_out[(y * LW + x) * 2 + 1]);
            assert(std::isfinite(r) && std::isfinite(g));
            assert(std::abs(r - peak) <= peak * 2e-3f + 1e-6f);
            assert(std::abs(g - average) <= average * 2e-3f + 1e-6f);
        }
    }
    assert(FromHalf(luma_out[(1 * LW + 3) * 2]) == 65504.0f);
    std::printf("HDR native shaders: luma (2x2 max and average, NaN, inf) PASS\n");
    Upload(vk, staging, scene, scene_half.data());
    reduce();

    const auto measure = [&](const Image& frame, bool copy_frame, uint32_t reset) {
        const Pass& pass = copy_frame ? snapshot : stats;
        std::vector<Resource> resources{{&frame}, {&luma_image}, {nullptr, &partials},
                                        {nullptr, &state}};
        if (copy_frame) {
            resources.push_back({&copy});
        }
        Bind(vk, pass, resources);
        Bind(vk, exposure, {{nullptr, &partials}, {nullptr, &state}});
        const ExposureParams params{groups_x * groups_y, reset, 64.0f, 0.1f, groups_x, 1e9f};
        Begin(vk);
        Dispatch(vk.cmd, pass, &reset, 4, groups_x, groups_y);
        Dispatch(vk.cmd, exposure, &params, sizeof(params), 1 + Tiles, 1);
        Submit(vk);
        ExposureState result;
        std::memcpy(&result, state.mapped, sizeof(result));
        return result;
    };
    std::vector<uint8_t> no_bloom = before_ui;
    for (uint32_t p = 0; p < W * H; ++p) {
        if (kinds[p] == Bloom || kinds[p] == Glow) {
            for (int c = 0; c < 3; ++c) {
                no_bloom[p * 4 + c] = Encode(Curve(Exposure * scene_rgb[p * 3 + c]));
            }
        }
    }
    Upload(vk, staging, target, no_bloom.data());
    ExposureState measured = measure(target, true, 1);
    std::printf("HDR native shaders: exposure %.4f stops (expected %.4f), %.0f samples\n",
                measured.log_scale, std::log2(Exposure), measured.samples);
    assert(measured.valid == 1.0f && measured.samples > 500.0f);
    assert(std::abs(measured.log_scale - std::log2(Exposure)) < 0.03f);
    const auto copied = Download<uint8_t>(vk, staging, copy);
    assert(copied == no_bloom);
    std::printf("HDR native shaders: snapshot copy and exposure PASS\n");

    std::memset(state.mapped, 0, sizeof(ExposureState));
    Upload(vk, staging, target, before_ui.data());
    measured = measure(target, false, 1);
    const float first_error = std::abs(measured.log_scale - std::log2(Exposure));
    for (int frame = 0; frame < 60; ++frame) {
        measured = measure(target, false, 0);
    }
    const float settled_error = std::abs(measured.log_scale - std::log2(Exposure));
    std::printf("HDR native shaders: with bloom %.3f stops off at first, %.3f after 60 frames\n",
                first_error, settled_error);
    assert(first_error > 0.1f && settled_error < 0.5f * first_error && settled_error < 0.15f);
    std::vector<uint8_t> black(W * H * 4, 0);
    Upload(vk, staging, target, black.data());
    const ExposureState kept = measure(target, false, 0);
    assert(kept.valid == 1.0f && kept.log_scale == measured.log_scale && kept.samples == 0.0f);
    const ExposureState dropped = measure(target, false, 1);
    assert(dropped.valid == 0.0f);
    std::printf("HDR native shaders: bloom-resistant exposure, empty frames PASS\n");

    Upload(vk, staging, target, no_bloom.data());
    measured = measure(target, true, 1);
    std::vector<uint8_t> finished = no_bloom;
    const auto ui = [](uint32_t x, uint32_t y) { return x >= 42 && x < 60 && y >= 22 && y < 26; };
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            if (ui(x, y)) {
                const uint32_t p = y * W + x;
                finished[p * 4] = 250;
                finished[p * 4 + 1] = 250;
                finished[p * 4 + 2] = uint8_t(finished[p * 4 + 2] ^ 0x5a);
            }
        }
    }
    for (uint32_t x = 40; x < 44; ++x) {
        finished[(12 * W + x) * 4] ^= 0x08;
    }
    Upload(vk, staging, target, finished.data());
    const float white = 1000.0f / 200.0f;
    for (const float strength : {1.0f, 0.5f}) {
        const GainParams params{strength, white, Knee, RollOffStart};
        Bind(vk, gain, {{&target}, {&copy}, {&luma_image}, {nullptr, &state}, {&gain_map}});
        Begin(vk);
        Dispatch(vk.cmd, gain, &params, sizeof(params), (W + 7) / 8, (H + 7) / 8);
        Submit(vk);
        const auto map = Download<uint16_t>(vk, staging, gain_map);
        const float scale = std::exp2(measured.log_scale);
        float light_gain = 0.0f;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                const uint32_t p = y * W + x;
                const float g = FromHalf(map[p]);
                const bool on_ui = ui(x, y) || (y == 12 && x >= 40 && x < 44);
                float sdr = 0.0f;
                for (int c = 0; c < 3; ++c) {
                    sdr = std::max(sdr, std::pow(no_bloom[p * 4 + c] / 255.0f, 2.2f));
                }
                if (on_ui) {
                    assert(g == -1.0f);
                } else if (sdr <= Knee) {
                    assert(g == 1.0f);
                } else {
                    assert(g >= 1.0f && sdr * g <= white * 1.001f);
                }
                if (kinds[p] == Light && !on_ui && x > 41 && x < 46 && y > 9 && y < 14) {
                    const float ratio = scale * 8.0f / Exposure / sdr;
                    const float expected = RollOff(sdr * std::pow(ratio, strength), white) / sdr;
                    if (std::abs(g - expected) > expected * 0.01f) {
                        std::fprintf(stderr, "gain %.4f, expected %.4f at %u,%u\n", g, expected,
                                     x, y);
                    }
                    assert(std::abs(g - expected) <= expected * 0.01f);
                    light_gain = g;
                }
            }
        }
        std::printf("HDR native shaders: gain map (strength %.2f: light x%.3f) PASS\n", strength,
                    light_gain);
    }
    {
        Upload(vk, staging, target, before_ui.data());
        measured = measure(target, true, 1);
        const GainParams params{1.0f, white, Knee, RollOffStart};
        Bind(vk, gain, {{&target}, {&copy}, {&luma_image}, {nullptr, &state}, {&gain_map}});
        Begin(vk);
        Dispatch(vk.cmd, gain, &params, sizeof(params), (W + 7) / 8, (H + 7) / 8);
        Submit(vk);
        const auto map = Download<uint16_t>(vk, staging, gain_map);
        uint32_t checked = 0;
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t x = 0; x < W; ++x) {
                if (kinds[y * W + x] == Bloom && (x <= 38 || x >= 49 || y <= 6 || y >= 17)) {
                    assert(FromHalf(map[y * W + x]) == 1.0f);
                    ++checked;
                }
            }
        }
        assert(checked >= 40);
        std::printf("HDR native shaders: bloom (SDR brighter than the scene) keeps gain 1 PASS\n");
    }
    {
        constexpr uint32_t W2 = 256, H2 = 192, LW2 = W2 / 2, LH2 = H2 / 2;
        const Image scene2 = MakeImage(vk, VK_FORMAT_R16G16B16A16_SFLOAT, W2, H2, 8);
        const Image luma2 = MakeImage(vk, VK_FORMAT_R16G16_SFLOAT, LW2, LH2, 4);
        const Image target2 = MakeImage(vk, VK_FORMAT_R8G8B8A8_UNORM, W2, H2, 4);
        const Image copy2 = MakeImage(vk, VK_FORMAT_R8G8B8A8_UNORM, W2, H2, 4);
        const Image gain2 = MakeImage(vk, VK_FORMAT_R16_SFLOAT, W2, H2, 2);
        const uint32_t gx = W2 / 16, gy = H2 / 16;
        const Buffer partials2 =
            MakeBuffer(vk, gx * gy * 16 * PartialStride, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const Buffer state2 = MakeBuffer(vk, sizeof(ExposureState), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const Buffer staging2 = MakeBuffer(vk, W2 * H2 * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Begin(vk);
        std::vector<VkImageMemoryBarrier> barriers;
        for (const Image* image : {&scene2, &luma2, &target2, &copy2, &gain2}) {
            barriers.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = image->image,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}});
        }
        vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                             uint32_t(barriers.size()), barriers.data());
        Submit(vk);
        std::vector<float> level2(W2 * H2);
        std::vector<float> sdr2(W2 * H2);
        const auto upload = [&] {
            std::vector<uint16_t> half(W2 * H2 * 4);
            std::vector<uint8_t> frame(W2 * H2 * 4);
            for (uint32_t p = 0; p < W2 * H2; ++p) {
                for (int c = 0; c < 3; ++c) {
                    half[p * 4 + c] = ToHalf(level2[p]);
                    frame[p * 4 + c] = Encode(sdr2[p]);
                }
                half[p * 4 + 3] = ToHalf(1.0f);
                frame[p * 4 + 3] = 255;
            }
            Upload(vk, staging2, scene2, half.data());
            Upload(vk, staging2, target2, frame.data());
            Bind(vk, luma, {{&scene2}, {&luma2}});
            Begin(vk);
            Dispatch(vk.cmd, luma, nullptr, 0, (LW2 + 7) / 8, (LH2 + 7) / 8);
            Submit(vk);
        };
        const auto run = [&](int frames, float min_tile) {
            Bind(vk, snapshot, {{&target2}, {&luma2}, {nullptr, &partials2}, {nullptr, &state2},
                                {&copy2}});
            Bind(vk, exposure, {{nullptr, &partials2}, {nullptr, &state2}});
            for (int frame = 0; frame < frames; ++frame) {
                const uint32_t reset = frame == 0 ? 1u : 0u;
                const ExposureParams params{gx * gy, reset, 64.0f, 0.1f, gx, min_tile};
                Begin(vk);
                Dispatch(vk.cmd, snapshot, &reset, 4, gx, gy);
                Dispatch(vk.cmd, exposure, &params, sizeof(params), 1 + Tiles, 1);
                Submit(vk);
            }
            ExposureState result;
            std::memcpy(&result, state2.mapped, sizeof(result));
            return result;
        };
        const auto gain_map = [&](const GainParams& params) {
            Bind(vk, gain, {{&target2}, {&copy2}, {&luma2}, {nullptr, &state2}, {&gain2}});
            Begin(vk);
            Dispatch(vk.cmd, gain, &params, sizeof(params), (W2 + 7) / 8, (H2 + 7) / 8);
            Submit(vk);
            return Download<uint16_t>(vk, staging2, gain2);
        };
        const auto decoded = [](float linear) { return std::pow(Encode(linear) / 255.0f, 2.2f); };
        const auto inverse = [](float sdr) {
            return sdr <= 0.5f ? sdr : 0.5f - 0.5f * std::log(1.0f - (sdr - 0.5f) / 0.5f);
        };

        std::memset(state2.mapped, 0, sizeof(ExposureState));
        for (uint32_t y = 0; y < H2; ++y) {
            for (uint32_t x = 0; x < W2; ++x) {
                const uint32_t p = y * W2 + x;
                const float t = float(x) / float(W2 - 1);
                float on_sdr_scale = 0.03f + 0.37f * t;
                float sdr = -1.0f;
                if (y >= 96 && y < 160) {
                    on_sdr_scale = 0.45f + 1.75f * t;
                } else if (y >= 160 && x < 128) {
                    on_sdr_scale = 0.02f;
                    sdr = 0.85f;
                }
                level2[p] = on_sdr_scale / Exposure;
                sdr2[p] = sdr >= 0.0f ? sdr : Curve(on_sdr_scale);
            }
        }
        upload();
        const ExposureState learned = run(60, 1e9f);
        assert(std::abs(learned.log_scale - std::log2(Exposure)) < 0.03f);
        uint32_t confident = 0;
        float previous = -100.0f;
        for (uint32_t b = 0; b < CurveBins; ++b) {
            const float center = CurveLow + (float(b) + 0.5f) * (CurveHigh - CurveLow) / CurveBins;
            const float* bin = learned.curve[b];
            assert(bin[3] >= previous - 1e-6f && bin[3] >= std::log2(center) - 1e-4f);
            previous = bin[3];
            if (bin[1] < 1.0f) {
                continue;
            }
            ++confident;
            const float expected = std::log2(inverse(center));
            if (std::abs(bin[3] - expected) > 0.12f) {
                std::fprintf(stderr, "curve bin %u: %.3f, expected %.3f\n", b, bin[3], expected);
            }
            assert(std::abs(bin[3] - expected) <= 0.12f);
        }
        std::printf("HDR native shaders: tone curve (%u of %u bins learned, the halo left out) PASS\n",
                    confident, CurveBins);
        assert(confident >= 14);
        {
            const float white = 5.0f;
            const auto map = gain_map({1.0f, white, Knee, RollOffStart});
            const float sdr = decoded(0.85f);
            const float ramp = Smoothstep(Knee, 1.0f, sdr);
            const float expected = RollOff(sdr * std::pow(inverse(sdr) / sdr, ramp), white) / sdr;
            float halo = 0.0f;
            for (uint32_t y = 166; y < 186; ++y) {
                for (uint32_t x = 8; x < 120; ++x) {
                    const float g = FromHalf(map[y * W2 + x]);
                    if (std::abs(g - expected) > expected * 0.04f) {
                        std::fprintf(stderr, "halo gain %.4f, expected %.4f at %u,%u\n", g,
                                     expected, x, y);
                    }
                    assert(std::abs(g - expected) <= expected * 0.04f);
                    halo = g;
                }
            }
            assert(halo > 1.15f);
            std::printf("HDR native shaders: bloom halo lifted by the curve (x%.3f) PASS\n", halo);
        }

        std::memset(state2.mapped, 0, sizeof(ExposureState));
        const float vignette = std::exp2(-0.5f);
        const auto light = [](uint32_t x, uint32_t y, uint32_t cx) {
            return x + 4 >= cx && x < cx + 4 && y + 4 >= 80 && y < 84;
        };
        const uint32_t left_x = 1 * 32 + 16, right_x = 6 * 32 + 16;
        for (uint32_t y = 0; y < H2; ++y) {
            for (uint32_t x = 0; x < W2; ++x) {
                const uint32_t p = y * W2 + x;
                const float on_sdr_scale = 0.03f + 0.37f * float((x * 7 + y * 3) % 64) / 63.0f;
                level2[p] = on_sdr_scale / Exposure;
                sdr2[p] = Curve(on_sdr_scale) * (x >= W2 / 2 ? vignette : 1.0f);
                if (light(x, y, left_x) || light(x, y, right_x)) {
                    level2[p] = 8.0f / Exposure;
                    sdr2[p] = 1.0f;
                }
            }
        }
        upload();
        const ExposureState tiled = run(40, 64.0f);
        for (uint32_t t = 0; t < Tiles; ++t) {
            const uint32_t tx = t % TilesX;
            const float expected = std::log2(Exposure) + (tx >= TilesX / 2 ? -0.5f : 0.0f);
            if (std::abs(tiled.tiles[t][0] - expected) > 0.06f || tiled.tiles[t][1] < 1.0f) {
                std::fprintf(stderr, "tile %u: %.3f (confidence %.2f), expected %.3f\n", t,
                             tiled.tiles[t][0], tiled.tiles[t][1], expected);
            }
            assert(std::abs(tiled.tiles[t][0] - expected) <= 0.06f && tiled.tiles[t][1] >= 1.0f);
        }
        {
            const float white = 40.0f;
            const auto map = gain_map({1.0f, white, Knee, RollOffStart});
            const float left = FromHalf(map[80 * W2 + left_x]);
            const float right = FromHalf(map[80 * W2 + right_x]);
            const float global = std::exp2(tiled.log_scale) * 8.0f / Exposure;
            std::printf("HDR native shaders: tiles: left light x%.3f, right x%.3f (one exposure: "
                        "x%.3f)\n",
                        left, right, global);
            assert(std::abs(left - 8.0f) <= 8.0f * 0.03f);
            assert(std::abs(right / left - vignette) <= 0.03f);
            assert(right < global * 0.9f);
            std::printf("HDR native shaders: local exposure (the vignette kept on highlights) PASS\n");
        }
    }
    vkDeviceWaitIdle(vk.device);
    std::printf("HDR native shaders: PASS\n");
    return 0;
}
