// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"

namespace VideoCore {
class BufferCache;
}

namespace Vulkan {

class Instance;
class Scheduler;

/**
 * Emulates IT_SET_PREDICATION and the pixel-pipe ZPASS statistics it consumes.
 *
 * On real hardware the CP evaluates the predicate when the predicated packets *execute*, long
 * after they were queued (applications never wait for occlusion results before enabling
 * predication). To match that without stalling, the predicate is built on the GPU timeline:
 * per-draw occlusion queries are reduced into a predicate buffer by a small compute pass and
 * predicated draws are wrapped in VK_EXT_conditional_rendering. The CPU never waits for query
 * results. Guest-visible ZPASS counter writeback is resolved asynchronously once the submission
 * that produced the samples completes.
 *
 * spartanport: unlike upstream shadPS4 this port records draws on a separate thread (the draw pipe).
 * The packet thread therefore only *captures* the predicate state (CapturePredicate) and hands it
 * to the recording thread, which performs the conditional rendering with no shared-state reads;
 * occlusion query slots are accounted for on the packet thread (PrepareDrawQuery) so a ZPASS dump
 * sees every query of its window.
 */
class PredicationManager {
public:
    explicit PredicationManager(const Instance& instance, Scheduler& scheduler,
                                VideoCore::BufferCache& buffer_cache);

    /// Predicate state captured on the packet thread for one predicated packet.
    struct PredicateState {
        bool gpu_active{}; ///< the predicate is built on the GPU timeline
        u32 slot{};        ///< predicate buffer slot when gpu_active
        bool inverted{};   ///< conditional rendering must be inverted (draw_visible == 0)
    };

    /// Returns true when the manager is active (SPARTAN_PREDICATION); else callers keep their own
    /// ZPASS counter simulation.
    bool Enabled() const {
        return enabled;
    }

    /// ZPASS counter pairs of the Neo/retail mode (set once by Liverpool).
    void SetCounterPairs(u32 pairs) {
        num_counter_pairs = pairs;
    }

    /// Handles PIXEL_PIPE_STAT_CONTROL: toggles perfect ZPASS sample counting.
    void ControlZpassCounting();

    /// Handles PIXEL_PIPE_STAT_RESET: clears the accumulated ZPASS sample counter.
    void ResetZpassCounting();

    /// Handles a ZPASS_DONE pixel-pipe stat dump. Captures the queries recorded since the last
    /// dump and schedules the guest counter writeback for when their results are available.
    void DumpZpassCounters(VAddr address);

    /// Enables predication from the ZPASS delta of an occlusion query results block.
    void EnableFromZpass(VAddr address, bool draw_visible, bool combine);

    /// Enables predication from a 32/64-bit boolean in guest memory, read on the GPU timeline.
    void EnableFromBool(VAddr address, bool is_64bit, bool draw_visible, bool combine);

    /// Disables predication; packets execute unconditionally.
    void Disable();

    /// Returns true when predicated packets must be skipped during command processing. Only
    /// possible when the predicate value is already final; GPU-side predicates never skip here.
    bool ShouldSkipPredicatedPacket() const {
        return mode == Mode::Static && !static_execute;
    }

    /// Returns true when predicated draws must be wrapped in conditional rendering.
    bool IsGpuPredicationActive() const {
        return mode == Mode::Gpu;
    }

    /// Snapshot of the current predicate for a draw about to be handed to the recording thread.
    PredicateState CapturePredicate() const {
        return {.gpu_active = mode == Mode::Gpu, .slot = gpu_slot, .inverted = gpu_inverted};
    }

    /// Acquires and resets an occlusion query slot for the next draw when counting is active.
    std::optional<u32> PrepareDrawQuery();

    /// Begins conditional rendering and/or the draw's occlusion query (recording thread).
    void BeginDraw(vk::CommandBuffer cmdbuf, std::optional<u32> query, bool predicated,
                   PredicateState state);

    /// Ends the draw's occlusion query and/or conditional rendering (recording thread).
    void EndDraw(vk::CommandBuffer cmdbuf, std::optional<u32> query, bool predicated,
                 PredicateState state);

private:
    enum class Mode {
        Disabled, ///< No predication; packets execute normally.
        Static,   ///< Predicate value is final; decision applied while parsing packets.
        Gpu,      ///< Predicate lives in the predicate buffer; draws use conditional rendering.
    };

    /// Queries captured between two pixel-pipe stat dumps, keyed by the dump address.
    struct DumpRecord {
        VAddr address{};
        u32 num_counter_pairs{};
        std::vector<u32> queries;
        bool poisoned{};
    };

    std::optional<u32> AcquireQuerySlot();
    void ReleaseQuerySlotsWhenDone(std::vector<u32>&& slots);
    void ResolveRecord(const DumpRecord& record);
    void WriteGuestCounters(VAddr address, u64 counter) const;
    std::optional<bool> ReadGuestZpass(VAddr address) const;

    void SetStatic(std::optional<bool> visible, bool draw_visible, bool combine);
    void BuildFromQueries(const std::vector<u32>& queries, bool draw_visible, bool combine);
    /// Picks (and seeds, when combining) the predicate slot the reduction writes into.
    struct SlotSelection {
        u32 slot{};
        bool combine_on_gpu{};
        std::optional<u32> seed;
    };
    SlotSelection SelectPredicateSlot(bool combine);
    void ActivateGpuPredicate(u32 slot, bool draw_visible);
    u32 AllocPredicateSlot();
    u32 AllocScratchQwords(u32 count);
    void ReducePredicate(u32 src_index, u32 count, const SlotSelection& selection);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::BufferCache& buffer_cache;
    bool enabled{}; ///< SPARTAN_PREDICATION
    bool supported{};

    // ZPASS sample counting.
    static constexpr u32 QueryPoolSize = 4096;
    vk::UniqueQueryPool query_pool;
    std::mutex query_mutex; ///< guards query_busy (packet thread vs deferred releases)
    std::array<bool, QueryPoolSize> query_busy{};
    u32 query_cursor{};
    bool counting_enabled{};
    bool active_poisoned{};
    std::vector<u32> active_queries;
    std::unordered_map<VAddr, std::shared_ptr<DumpRecord>> records;
    u64 zpass_counter{};
    u32 num_counter_pairs{};

    // GPU predicate generation.
    static constexpr u32 NumPredicateSlots = 1024;
    static constexpr u32 NumScratchQwords = 8192;
    VideoCore::Buffer predicate_buffer;
    VideoCore::Buffer counter_scratch;
    u32 predicate_cursor{};
    u32 scratch_cursor{};
    vk::UniqueDescriptorSetLayout reduce_desc_layout;
    vk::UniquePipelineLayout reduce_pipeline_layout;
    vk::UniquePipeline reduce_pipeline;

    // Current predication state (touched on the packet thread only).
    Mode mode{Mode::Disabled};
    bool static_execute{true};
    std::optional<bool> static_visible;
    u32 gpu_slot{};
    bool gpu_inverted{};
};

} // namespace Vulkan
