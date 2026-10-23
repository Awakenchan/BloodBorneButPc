// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: native HDR output (hdr_mode=native, docs/hdr.md). The HDR encode (hdr_output.frag)
// shows the game's finished SDR frame, UI included; native mode brightens its highlights by how
// much brighter the game's own linear HDR scene is where the tonemap compressed them, measured
// per frame on the GPU (hdr_native.comp) at three points of Bloodborne's frame that the temporal
// upscaler already finds (vk_temporal_upscaler.h):
// - before the post-processing combine (compute shader 9a9cf8a9, after the upscaler wrote its
//   output into the scene color): the scene's brightest channel at half size (OnSceneColor);
// - at the first UI pass (the first Scaleform draw, vertex shader 34e8a281 or another of
//   UiComposition::MovieShader; with the scaled presets the upscaled output-size UI image): a
//   copy of the tonemapped frame before any UI pixel, and the exposure the game's curve gave the
//   midtones (OnUiStart);
// - at the display pass that copies the finished frame into a display buffer (the first pass of
//   the next frame): the gain map, 1 below the knee and where the frame differs from the copy
//   (UI). It belongs to that display buffer: the presenter takes it when the buffer is flipped,
//   which comes a frame later in the command stream, so a frame and its map always match.
// A frame missing any of them (title, menus, loading, movies) has no map: the encode falls back
// to expand mode for it. All passes are recorded on the recording threads (Scheduler::Record), a
// handful of dispatches per frame, nothing per draw; no readback and no wait.

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;

/// A display buffer's gain map: R16F at the finished frame's size, General layout; negative
/// where the frame had UI (magnitude 1 there). Handed from the display pass to the presenter's
/// Frame by exchange (HdrNative::TakeGainMap), never copied.
struct HdrGainMap {
    VideoCore::UniqueImage image;
    vk::UniqueImageView view;
    u32 width = 0, height = 0;
};

class HdrNative {
public:
    HdrNative(const Instance& instance, Scheduler& scheduler,
              VideoCore::TextureCache& texture_cache, Runtime& runtime);
    ~HdrNative();

    /// Native mode is in use: an HDR swapchain is presented and hdr_mode is native. Otherwise
    /// nothing is recorded.
    [[nodiscard]] static bool Wanted();

    /// An image a pass reads: in General layout by the time the commands run, `view` sampled
    /// (UNORM for the display-encoded frames).
    struct Source {
        vk::Image image;
        vk::ImageView view;
        u32 width = 0, height = 0;
    };
    /// A guest image as a Source: transitioned by the runtime (a reduced-size scene proxy is
    /// resolved into it first), viewed as `format`.
    Source Guest(VideoCore::ImageId id, vk::Format format);

    /// Before the post-processing combine: the scene color (RGBA16F), upscaled by now. Once a
    /// frame.
    void OnSceneColor(VideoCore::ImageId scene);
    /// The first UI pass is next and this frame's scene was seen: OnUiStart is wanted.
    [[nodiscard]] bool WantsUiStart() const noexcept {
        return luma_ready && !ui_started;
    }
    /// The tonemapped frame before the first UI pass (empty: none found). Once a frame.
    void OnUiStart(const Source& frame);
    /// The display pass copying the finished frame `frame` (empty: none) into the display buffer
    /// at `address`: its gain map, or none. Ends the frame.
    void OnDisplayPass(VAddr address, const Source& frame);

    /// Presenter, when the display buffer at `address` is flipped: exchanges `map` with the gain
    /// map made for it, if it has one (then true). The display buffer's slot gets the old map
    /// (`map` may be null): the frame it came from has been presented (its fence waited).
    bool TakeGainMap(VAddr address, std::unique_ptr<HdrGainMap>& map);

private:
    struct OwnImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0;
    };
    struct Buffer {
        vk::Buffer buffer{};
        VmaAllocation allocation{};
        vk::DeviceSize size = 0;
    };
    struct Slot {
        std::unique_ptr<HdrGainMap> map;
        bool valid = false; ///< made for the frame the display buffer holds now
    };

    void CreatePipelines();
    /// (Re)creates `image` at `width` x `height`; the old one goes once the GPU is past it.
    void EnsureImage(OwnImage& image, u32 width, u32 height, vk::Format format, const char* name);
    void EnsureGainMap(std::unique_ptr<HdrGainMap>& map, u32 width, u32 height);
    /// Room for `groups` partial sums of the STATS pass.
    void EnsurePartials(u32 groups);
    /// The exposure statistics of `frame` against this frame's luma (`copy`: also copying it
    /// into the snapshot), then the exposure update.
    void RecordExposure(const Source& frame, bool copy);
    void RecordGain(const Source& frame, const Source& before_ui, const HdrGainMap& map);
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format);
    void EndFrame();

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;

    vk::UniqueDescriptorSetLayout luma_layout, stats_layout, snapshot_layout, exposure_layout,
        gain_layout;
    vk::UniquePipelineLayout luma_pipeline_layout, stats_pipeline_layout,
        snapshot_pipeline_layout, exposure_pipeline_layout, gain_pipeline_layout;
    vk::UniquePipeline luma_pipeline, stats_pipeline, snapshot_pipeline, exposure_pipeline,
        gain_pipeline;

    OwnImage luma;     ///< RG16F, half the scene's size: the brightest channel's 2x2 max, average
    OwnImage snapshot; ///< RGBA8, the tonemapped frame before the UI
    Buffer partials;   ///< per STATS group: the exposure sums and the tone curve's bin sums
    Buffer state;      ///< hdr_native.comp's state (exposure, tone curve, tiles), zero at start
    bool state_cleared = false;
    /// The exposure was measured in the last frame (else the next measurement replaces it).
    bool measured_last_frame = false;

    // This frame (from one display pass to the next), on the draw recording thread.
    bool luma_ready = false;     ///< the scene was reduced
    bool ui_started = false;     ///< OnUiStart was called
    bool snapshot_ready = false; ///< the UI-free copy and the exposure were taken
    vk::Image snapshot_source{}; ///< the image they were taken from

    std::mutex slots_mutex; ///< TakeGainMap runs where the flip is handled
    std::unordered_map<VAddr, Slot> slots;

    struct ViewEntry {
        vk::Image image;
        u64 uid = 0;
        vk::Format format{};
        vk::ImageView view;
        u64 last_use = 0;
    };
    std::array<ViewEntry, 4> view_cache{};
    u64 view_uses = 0;
};

} // namespace Vulkan
