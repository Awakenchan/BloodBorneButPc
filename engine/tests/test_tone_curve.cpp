// SPDX-License-Identifier: GPL-3.0-or-later
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "video_core/host_shaders/fs_tri_vert.h"

namespace {

#define CHECK(call)                                                                               \
    do {                                                                                          \
        const VkResult result_ = (call);                                                          \
        if (result_ != VK_SUCCESS) {                                                              \
            std::fprintf(stderr, "%s failed: %d\n", #call, int(result_));                         \
            std::exit(1);                                                                         \
        }                                                                                         \
    } while (0)

struct ToneCurvePush {
    float knee, peak, strength, enabled;
};
constexpr uint32_t PushBytes = 128 + sizeof(ToneCurvePush);

struct Vk {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = 0;
    VkCommandPool pool{};
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

Vk MakeVk() {
    Vk vk;
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                             .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&instance_info, nullptr, &vk.instance));
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vk.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(vk.instance, &count, devices.data());
    if (devices.empty()) {
        std::fprintf(stderr, "no Vulkan device\n");
        std::exit(1);
    }
    vk.physical = devices[0];
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            vk.physical = device;
            break;
        }
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(vk.physical, &properties);
    std::printf("Device: %s\n", properties.deviceName);
    vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            vk.family = i;
            break;
        }
    }
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features v13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                         .pNext = &barycentric};
    VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                         .pNext = &v13};
    VkPhysicalDeviceVulkan11Features v11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                         .pNext = &v12};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &v11};
    vkGetPhysicalDeviceFeatures2(vk.physical, &features);
    if (!v13.dynamicRendering || !v12.uniformBufferStandardLayout) {
        std::fprintf(stderr, "the device lacks dynamic rendering or standard uniform layout\n");
        std::exit(1);
    }
    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(vk.physical, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(vk.physical, nullptr, &extension_count, extensions.data());
    std::vector<const char*> enabled;
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) == 0) {
            enabled.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
        }
    }
    if (enabled.empty()) {
        v13.pNext = nullptr;
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                             .queueFamilyIndex = vk.family,
                                             .queueCount = 1,
                                             .pQueuePriorities = &priority};
    const VkDeviceCreateInfo device_info{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                         .pNext = &features,
                                         .queueCreateInfoCount = 1,
                                         .pQueueCreateInfos = &queue_info,
                                         .enabledExtensionCount = uint32_t(enabled.size()),
                                         .ppEnabledExtensionNames = enabled.data()};
    CHECK(vkCreateDevice(vk.physical, &device_info, nullptr, &vk.device));
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                            .queueFamilyIndex = vk.family};
    CHECK(vkCreateCommandPool(vk.device, &pool_info, nullptr, &vk.pool));
    return vk;
}

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped = nullptr;
};

Buffer MakeBuffer(const Vk& vk, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer buffer;
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    CHECK(vkCreateBuffer(vk.device, &info, nullptr, &buffer.buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk.device, buffer.buffer, &requirements);
    const VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = vk.MemoryType(requirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(vk.device, &alloc, nullptr, &buffer.memory));
    CHECK(vkBindBufferMemory(vk.device, buffer.buffer, buffer.memory, 0));
    CHECK(vkMapMemory(vk.device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped));
    return buffer;
}

std::vector<uint32_t> ReadWords(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
}

struct Ramp {
    const Vk& vk;
    uint32_t width = 0;
    bool uniform = true;
    VkDescriptorSetLayout set_layout{};
    VkPipelineLayout layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet set{};
    VkImage image{};
    VkDeviceMemory image_memory{};
    VkImageView view{};
    Buffer constants, readback;
    VkPipeline game{}, hdr{};

    VkPipeline Pipeline(const std::vector<uint32_t>& fragment) const {
        VkShaderModule modules[2];
        const VkShaderModuleCreateInfo vert{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = sizeof(FS_TRI_VERT),
                                            .pCode = FS_TRI_VERT};
        const VkShaderModuleCreateInfo frag{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = fragment.size() * 4,
                                            .pCode = fragment.data()};
        CHECK(vkCreateShaderModule(vk.device, &vert, nullptr, &modules[0]));
        CHECK(vkCreateShaderModule(vk.device, &frag, nullptr, &modules[1]));
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main"},
        };
        const VkPipelineVertexInputStateCreateInfo vertex_input{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        const VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        const VkViewport viewport{0, 0, float(width), 1, 0, 1};
        const VkRect2D scissor{{0, 0}, {width, 1}};
        const VkPipelineViewportStateCreateInfo viewport_state{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
        const VkPipelineRasterizationStateCreateInfo raster{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        const VkPipelineColorBlendAttachmentState attachment{.colorWriteMask = 0xF};
        const VkPipelineColorBlendStateCreateInfo blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &attachment};
        const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
        const VkPipelineRenderingCreateInfo rendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
        const VkGraphicsPipelineCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &rendering,
            .stageCount = 2, .pStages = stages,
            .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly,
            .pViewportState = &viewport_state, .pRasterizationState = &raster,
            .pMultisampleState = &multisample, .pColorBlendState = &blend,
            .layout = layout};
        VkPipeline pipeline;
        CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
        vkDestroyShaderModule(vk.device, modules[0], nullptr);
        vkDestroyShaderModule(vk.device, modules[1], nullptr);
        return pipeline;
    }

    void Make(const std::vector<uint32_t>& game_code, const std::vector<uint32_t>& hdr_code) {
        const VkDescriptorType type = uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                              : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        const VkDescriptorSetLayoutBinding binding{0, type, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        const VkDescriptorSetLayoutCreateInfo set_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1, .pBindings = &binding};
        CHECK(vkCreateDescriptorSetLayout(vk.device, &set_info, nullptr, &set_layout));
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       PushBytes};
        const VkPipelineLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &set_layout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
        CHECK(vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &layout));
        const VkDescriptorPoolSize size{type, 1};
        const VkDescriptorPoolCreateInfo pool_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
        CHECK(vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &descriptor_pool));
        const VkDescriptorSetAllocateInfo alloc{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
        CHECK(vkAllocateDescriptorSets(vk.device, &alloc, &set));
        constants = MakeBuffer(vk, 65536, uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                                  : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memset(constants.mapped, 0, 65536);
        const VkDescriptorBufferInfo buffer_info{constants.buffer, 0, 65536};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
                                         .descriptorType = type, .pBufferInfo = &buffer_info};
        vkUpdateDescriptorSets(vk.device, 1, &write, 0, nullptr);

        const VkImageCreateInfo image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .extent = {width, 1, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        CHECK(vkCreateImage(vk.device, &image_info, nullptr, &image));
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(vk.device, image, &requirements);
        const VkMemoryAllocateInfo image_alloc{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = vk.MemoryType(requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(vk.device, &image_alloc, nullptr, &image_memory));
        CHECK(vkBindImageMemory(vk.device, image, image_memory, 0));
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(vk.device, &view_info, nullptr, &view));
        readback = MakeBuffer(vk, VkDeviceSize(width) * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        game = Pipeline(game_code);
        hdr = Pipeline(hdr_code);
    }

    std::vector<float> Draw(VkPipeline pipeline, const ToneCurvePush& tone) const {
        VkCommandBuffer cmd;
        const VkCommandBufferAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .commandPool = vk.pool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1};
        CHECK(vkAllocateCommandBuffers(vk.device, &alloc, &cmd));
        const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkBeginCommandBuffer(cmd, &begin));
        VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = image,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        const VkRenderingAttachmentInfo color{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                              .imageView = view,
                                              .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                              .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .renderArea = {{0, 0}, {width, 1}},
                                        .layerCount = 1,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &color};
        vkCmdBeginRendering(cmd, &rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        std::array<uint8_t, PushBytes> push{};
        std::memcpy(push.data() + 128, &tone, sizeof(tone));
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           PushBytes, push.data());
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .imageExtent = {width, 1, 1}};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                               &copy);
        CHECK(vkEndCommandBuffer(cmd));
        const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                  .commandBufferCount = 1,
                                  .pCommandBuffers = &cmd};
        CHECK(vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(vk.queue));
        vkFreeCommandBuffers(vk.device, vk.pool, 1, &cmd);
        std::vector<float> row(size_t(width) * 4);
        std::memcpy(row.data(), readback.mapped, row.size() * 4);
        return row;
    }
};

int failures = 0;

void Fail(const std::string& what) {
    std::printf("  FAIL %s\n", what.c_str());
    ++failures;
}

void Check(Ramp& ramp, const std::string& name, uint32_t k_word, uint32_t shoulder_word, float k,
           float shoulder, float low, float stops) {
    auto* words = static_cast<float*>(ramp.constants.mapped);
    std::memset(words, 0, 65536);
    words[k_word] = k;
    words[shoulder_word] = shoulder;
    const uint32_t w = ramp.width;
    const auto game = ramp.Draw(ramp.game, {0.75f, 5.0f, 1.0f, 1.0f});
    const auto off = ramp.Draw(ramp.hdr, {0.75f, 5.0f, 1.0f, 0.0f});
    if (std::memcmp(game.data(), off.data(), game.size() * 4) != 0) {
        Fail(name + ": extension off differs from the game's curve");
    }
    struct Case {
        float knee, peak, strength;
    };
    for (const Case c : {Case{0.75f, 5.0f, 1.0f}, Case{0.6f, 10.0f, 0.5f}, Case{0.85f, 2.5f, 1.0f}}) {
        const auto on = ramp.Draw(ramp.hdr, {c.knee, c.peak, c.strength, 1.0f});
        const float limit = std::pow(c.peak, c.strength);
        char label[96];
        std::snprintf(label, sizeof(label), "%s knee %.2f peak %.1f strength %.1f", name.c_str(),
                      c.knee, c.peak, c.strength);
        for (int channel = 0; channel < 3; ++channel) {
            uint32_t first_above = w;
            for (uint32_t i = 0; i < w; ++i) {
                const float g = game[i * 4 + channel], h = on[i * 4 + channel];
                if (g < c.knee * 0.999f) {
                    if (std::memcmp(&g, &h, 4) != 0) {
                        Fail(std::string(label) + ": differs below the knee at " + std::to_string(i));
                        break;
                    }
                } else if (first_above == w && h != g) {
                    first_above = i;
                }
            }
            if (first_above == w || first_above < 3 || first_above + 3 >= w) {
                Fail(std::string(label) + ": no knee found on the ramp");
                continue;
            }
            float previous = on[(first_above - 1) * 4 + channel];
            float largest = previous;
            for (uint32_t i = first_above; i < w; ++i) {
                const float h = on[i * 4 + channel];
                if (!(h >= previous) || !std::isfinite(h)) {
                    Fail(std::string(label) + ": not monotonic at " + std::to_string(i));
                    break;
                }
                previous = h;
                largest = std::max(largest, h);
            }
            if (!(largest < limit)) {
                Fail(std::string(label) + ": reaches the peak (" + std::to_string(largest) + ")");
            }
            const float dx = stops / float(w);
            const auto slope = [&](uint32_t i) {
                return (std::log2(on[(i + 1) * 4 + channel]) - std::log2(on[i * 4 + channel])) / dx;
            };
            const float below = slope(first_above - 3), above = slope(first_above + 1);
            const float jump = std::abs(std::log2(on[first_above * 4 + channel]) -
                                        std::log2(on[(first_above - 1) * 4 + channel]));
            if (std::abs(above - below) > 0.05f * std::max(std::abs(below), 0.05f) + 0.01f ||
                jump > 4.0f * std::max(std::abs(below), 0.05f) * dx + 1e-4f) {
                char line[160];
                std::snprintf(line, sizeof(line), ": not smooth at the knee (slope %.4f -> %.4f, step %.5f)",
                              below, above, jump);
                Fail(std::string(label) + line);
            }
            if (channel == 0) {
                const float x_knee = std::exp2(low + stops * (float(first_above) + 0.5f) / float(w));
                std::printf("  %s: knee at scene %.4f, slope %.3f, %.3f at the ramp's end (limit %.3f)\n",
                            label, x_knee, below, on[(w - 1) * 4], limit);
            }
        }
    }
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: tone-curve-shader-test <ramp folder>...\n");
        return 2;
    }
    Vk vk = MakeVk();
    for (int i = 1; i < argc; ++i) {
        const std::string dir = argv[i];
        std::ifstream meta(dir + "/ramp.txt");
        std::string family, type;
        uint32_t k_word = 0, shoulder_word = 0, width = 0;
        float low = 0, stops = 0;
        if (!(meta >> family >> k_word >> shoulder_word >> low >> stops >> width >> type)) {
            std::fprintf(stderr, "%s: no ramp.txt\n", dir.c_str());
            return 2;
        }
        Ramp ramp{vk};
        ramp.width = width;
        ramp.uniform = type == "uniform";
        ramp.Make(ReadWords(dir + "/game.spv"), ReadWords(dir + "/hdr.spv"));
        const std::string name = dir.substr(dir.find_last_of("/\\") + 1);
        std::printf("%s (%s)\n", name.c_str(), family.c_str());
        if (family == "exponential") {
            Check(ramp, name, k_word, shoulder_word, 1.0f, 0.1f, low, stops);
            Check(ramp, name + " c=0", k_word, shoulder_word, 2.0f, 0.0f, low, stops);
        } else {
            Check(ramp, name, k_word, shoulder_word, 1.0f, 1.0f / 16.0f, low, stops);
            Check(ramp, name + " w=0", k_word, shoulder_word, 1.0f, 0.0f, low, stops);
        }
    }
    std::printf(failures ? "Tone curve: %d failures\n" : "Tone curve: all checks passed\n", failures);
    return failures ? 1 : 0;
}
