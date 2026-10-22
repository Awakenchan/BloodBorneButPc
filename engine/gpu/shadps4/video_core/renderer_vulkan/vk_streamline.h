// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: NVIDIA DLSS Frame Generation (DLSS-G) through NVIDIA Streamline
// (docs/dlss_frame_generation.md). frame_gen=dlss at start loads sl.interposer.dll (next to the
// engine, NVIDIA-signed) before the Vulkan instance: its vkGetInstanceProcAddr is the engine's
// Vulkan loader from then on, so the instance, the device and the swapchain go through
// Streamline's proxies, which add what its plugins need and let DLSS-G present the real and the
// generated frames. Reflex runs with it, as NVIDIA requires.
//
// Per game frame: the GPU command thread numbers it (Reflex sleep and markers), the frame
// generator's captures (depth, motion vectors, the frame without its UI) are copied into the
// presentation frame's slot, and on the swap thread the frame's constants and tags go out right
// before its present, with a UI alpha image made from the back buffer and the HUD-less frame.
// Frames without those inputs (menus, movies, loading) have DLSS-G's mode off.
//
// Windows only (Streamline has no Linux runtime); without its files, on a GPU without DLSS-G or
// with OptiScaler loaded the engine starts on vulkan-1.dll as before.

#pragma once

#include <array>
#include <chrono>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_streamline_loader.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;

/// bbport: DLSS-G with HDR output: the presenter encodes the frame without its UI as it encodes
/// the back buffer (PresentHdr), so the two differ only in the UI, and hands it over as with SDR.
/// BB_DLSSG_HDR=0: no generated frames with HDR output, as before 0.9.37 beta 9.
bool DlssFrameGenHdr();

/// One game frame's DLSS-G inputs: with its presentation frame to the swap thread.
struct DlssFrameInfo {
    u32 index = 0;      ///< Streamline's frame index (0: Streamline is not loaded)
    bool inputs = false; ///< depth, motion vectors and the HUD-less frame are in slot `slot`
    u32 slot = 0;
    u32 render_width = 0, render_height = 0;
    std::array<float, 2> jitter{};
    float near_plane = 0.1f, vertical_fov = 1.0f;
    bool reset = true;
    CameraMotion::Matrices camera{};
};

class DlssFrameGen {
public:
    /// `slots`: one per presentation frame (the frame's id picks its slot).
    DlssFrameGen(const Instance& instance, u32 slots);
    ~DlssFrameGen();

    /// Streamline is loaded and DLSS-G runs on this GPU.
    [[nodiscard]] bool Available() const;

    // GPU command thread.

    /// The index of the game frame being recorded (its Reflex sleep and first markers went out
    /// with the first call), 0 without Streamline.
    u32 CurrentFrame();
    /// After the frame's submission (PrepareFrame): its render submit ended; the next one begins.
    void OnFrameSubmitted();
    /// Makes `slot`'s depth and motion images `w` x `h` (waits for the GPU when they change).
    void EnsureSlot(u32 slot, u32 w, u32 h);
    /// Records the copies of the captured depth (R32F) and motion vectors (RG16F), General
    /// layout, into `slot` (a recording thread).
    void RecordInputs(vk::CommandBuffer cmdbuf, u32 slot, vk::Image depth, vk::Image motion,
                      u32 w, u32 h) const;

    // Swap thread.

    /// Into the present command buffer, once the back buffer is complete (in PresentSrc): the
    /// HUD-less frame at the back buffer's size and the UI alpha image (1 where the back buffer
    /// differs from it: the game's UI and the engine's overlay). `hudless` is the HUD-less frame
    /// through the post process (General, `frame_extent`, the back buffer's format), placed in
    /// the back buffer as the frame is (`fit`).
    void RecordPresent(vk::CommandBuffer cmdbuf, const DlssFrameInfo& info, vk::Image hudless,
                       vk::ImageView hudless_view, vk::Extent2D frame_extent,
                       vk::Image backbuffer, vk::Format format, vk::Extent2D extent,
                       vk::Rect2D fit);
    /// Right before vkQueuePresentKHR: DLSS-G's mode, the frame's constants and tags, the present
    /// start marker. `info` null: not a game frame.
    void BeforePresent(const DlssFrameInfo* info, bool menu_open, bool hdr);
    /// Right after vkQueuePresentKHR: the present end marker, DLSS-G's state and FPS.
    void AfterPresent(const DlssFrameInfo* info);
    /// Before the swapchain is made again (a resize, a fullscreen switch): DLSS-G off first.
    void BeforeSwapchainChange();

private:
    struct Slot {
        VideoCore::UniqueImage depth, motion;
        vk::UniqueImageView depth_view, motion_view;
        u32 width = 0, height = 0;
    };
    void CreatePipeline();
    void EnsurePresentImages(vk::Format format, vk::Extent2D extent);
    void SetMode(bool on);

    const Instance& instance;
    std::vector<Slot> slots;

    // GPU command thread.
    u32 current_frame = 0;

    // Swap thread.
    VideoCore::UniqueImage hudless_image, final_image, ui_image;
    vk::UniqueImageView hudless_view, final_view, ui_view;
    vk::Format present_format{};
    vk::Extent2D present_extent{};
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline ui_pipeline;
    /// What RecordPresent made for this present: the HUD-less image to tag (the frame's own when
    /// it covers the back buffer) and whether the UI alpha image is valid.
    vk::Image tag_hudless{};
    vk::ImageView tag_hudless_view{};
    bool tag_ui = false;
    bool mode_on = false;
    int applied_frames = 0;
    bool applied_dynamic = false;  ///< bbport: DLSSGMode::eDynamic given
    int applied_target_fps = 0;
    /// What the options were last given: the back buffer's format and size, the render size.
    vk::Format options_format{};
    vk::Extent2D options_extent{}, options_render{}, options_applied_render{};
    double presented_avg = 1.0;
    std::chrono::steady_clock::time_point stats_start{};
    u32 stats_frames = 0, stats_presented = 0;
    int applied_reflex = -1;
    bool failed = false;
    std::chrono::steady_clock::time_point last_present{};
    double interval_ms = 0.0;
    u32 status_logged = 0;
};

} // namespace Vulkan
