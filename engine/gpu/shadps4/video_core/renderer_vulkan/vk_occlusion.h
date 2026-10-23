// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <unordered_map>

#include "common/types.h"
#include "video_core/renderer_vulkan/occlusion_model.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
class Rasterizer;

class Occlusion {
public:
    static bool Wanted();

    static std::unique_ptr<Occlusion> Create(const Instance& instance, Scheduler& scheduler,
                                             Rasterizer& rasterizer);
    ~Occlusion();

    void NoteIssued() noexcept {
        ++decoded_draws;
    }

    struct DumpTask {
        VAddr va;
        u64 issued;
        u64 frame;
        u32 pipes;
        u32 pad;
    };
    DumpTask MakeDump(VAddr va, u32 num_pipes) const noexcept;

    struct PredicationTask {
        VAddr block;
        u32 op;
        u8 draw_if_visible;
        u8 cont;
        u16 pad;
    };
    bool predication_open = false;

    bool HasUnsubmittedDumps(u64 current_tick) const noexcept {
        return last_dump_tick == current_tick;
    }

    void Dump(const DumpTask& task);
    void SetPredication(const PredicationTask& task);

    bool SkipDraw() noexcept {
        if (predicate == Predicate::None) [[likely]] {
            return false;
        }
        return SkipUnderPredicate();
    }

    struct DrawQuery {
        vk::QueryPool pool{};
        u32 index = 0;
        vk::QueryControlFlags flags{};
    };
    DrawQuery BeginDraw(u32 scene_size) noexcept {
        if (!Measuring()) [[likely]] {
            return {};
        }
        return TakeQuery(scene_size);
    }

    [[nodiscard]] bool Measuring() const noexcept {
        return !open_blocks.Empty() && open_blocks.WantsQuery(next_query, draw_budget);
    }

private:
    struct Resolver;

    Occlusion(const Instance& instance, Scheduler& scheduler, Rasterizer& rasterizer);
    bool Init();
    DrawQuery TakeQuery(u32 scene_size) noexcept;
    bool SkipUnderPredicate() noexcept;

    static constexpr u32 RingSize = 16384;
    static constexpr u64 OpenFrames = 8;

    const Instance& instance;
    Scheduler& scheduler;
    Rasterizer& rasterizer;
    vk::QueryControlFlags query_flags{};
    u64 draw_budget = 64;

    u64 decoded_draws = 0;

    OcclusionModel::OpenBlocks open_blocks;
    u64 next_query = 0;
    u64 dumps_recorded = 0;
    u64 last_dump_tick = ~0ull;
    struct LastDump {
        u64 number;
        u32 dbs;
    };
    std::unordered_map<VAddr, LastDump> last_dump;
    enum class Predicate : u8 { None, Skip, Draw };
    Predicate predicate = Predicate::None;
    bool chain_seen = false;
    bool chain_unknown = false;

    std::shared_ptr<Resolver> resolver;
};

}
