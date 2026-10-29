// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <source_location>
#include <utility>
#include <vector>
#include <mutex>
#include <thread>
#include <queue>

#include "spartanport_toggles.h"
#include "common/assert.h"
#include "common/interval_set.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_recording_chunk_holder.h"
#include "video_core/renderer_vulkan/vk_recording_observer.h"
#include "video_core/renderer_vulkan/vk_recording_ownership.h"
#include "video_core/renderer_vulkan/vk_render_pass_pairing.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;

struct RenderAttachment {
    vk::ImageView image_view;
    vk::ImageLayout image_layout;
    std::array<u32, 4> clear_value;
    union {
        u32 is_clear;
        struct {
            bool has_depth;
            bool depth_clear;
            bool has_stencil;
            bool stencil_clear;
        };
    };
};
static_assert(std::has_unique_object_representations_v<RenderAttachment>);

struct RenderState {
    std::array<RenderAttachment, 8> color_attachments;
    RenderAttachment depth_stencil_attachment;
    u16 width;
    u16 height;
    u16 num_layers;
    u16 num_color_attachments;

    bool operator==(const RenderState& other) const noexcept {
        return std::memcmp(this, &other, sizeof(RenderState)) == 0;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

struct SubmitInfo {
    std::array<vk::Semaphore, 4> wait_semas;
    std::array<u64, 4> wait_ticks;
    std::array<vk::Semaphore, 4> signal_semas;
    std::array<u64, 4> signal_ticks;
    vk::Fence fence;
    u32 num_wait_semas;
    u32 num_signal_semas;

    void AddWait(vk::Semaphore semaphore, u64 tick = 1) {
        wait_semas[num_wait_semas] = semaphore;
        wait_ticks[num_wait_semas++] = tick;
    }

    void AddSignal(vk::Semaphore semaphore, u64 tick = 1) {
        signal_semas[num_signal_semas] = semaphore;
        signal_ticks[num_signal_semas++] = tick;
    }

    void AddSignal(vk::Fence fence) {
        this->fence = fence;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        return fail_op == other.fail_op && pass_op == other.pass_op &&
               depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
    }
};
struct DynamicState {
    struct {
        bool viewports : 1;
        bool scissors : 1;

        bool depth_test_enabled : 1;
        bool depth_write_enabled : 1;
        bool depth_compare_op : 1;

        bool depth_bounds_test_enabled : 1;
        bool depth_bounds : 1;

        bool depth_bias_enabled : 1;
        bool depth_bias : 1;

        bool stencil_test_enabled : 1;
        bool stencil_front_ops : 1;
        bool stencil_front_reference : 1;
        bool stencil_front_write_mask : 1;
        bool stencil_front_compare_mask : 1;
        bool stencil_back_ops : 1;
        bool stencil_back_reference : 1;
        bool stencil_back_write_mask : 1;
        bool stencil_back_compare_mask : 1;

        bool primitive_restart_enable : 1;
        bool rasterizer_discard_enable : 1;
        bool cull_mode : 1;
        bool front_face : 1;

        bool blend_constants : 1;
        bool color_write_masks : 1;
        bool line_width : 1;
        bool feedback_loop_enabled : 1;
    } dirty_state{};

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    /// Commits the dynamic state to the provided command buffer (a null one only clears the
    /// flags that committing clears).
    void Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf);

    /// spartanport: passes each dirty state change to `emit` as a small command closure holding only
    /// its own values (recording a copy of the whole state per draw was a hot spot), and clears
    /// the flags it emitted. Flags whose test is disabled stay dirty, as in Commit.
    template <typename Emit>
    void CommitWith(bool depth_bounds_supported, bool color_write_mask_supported,
                    bool feedback_loop_supported, Emit&& emit) {
        using Face = vk::StencilFaceFlagBits;
        if (dirty_state.viewports) {
            dirty_state.viewports = false;
            emit([v = viewports](vk::CommandBuffer c) { c.setViewportWithCount(v); });
        }
        if (dirty_state.scissors) {
            dirty_state.scissors = false;
            emit([v = scissors](vk::CommandBuffer c) { c.setScissorWithCount(v); });
        }
        if (dirty_state.depth_test_enabled) {
            dirty_state.depth_test_enabled = false;
            emit([v = depth_test_enabled](vk::CommandBuffer c) { c.setDepthTestEnable(v); });
        }
        if (dirty_state.depth_write_enabled) {
            // Must be set in a command buffer even if depth test is disabled.
            dirty_state.depth_write_enabled = false;
            emit([v = depth_write_enabled](vk::CommandBuffer c) { c.setDepthWriteEnable(v); });
        }
        if (depth_test_enabled && dirty_state.depth_compare_op) {
            dirty_state.depth_compare_op = false;
            emit([v = depth_compare_op](vk::CommandBuffer c) { c.setDepthCompareOp(v); });
        }
        if (dirty_state.depth_bounds_test_enabled) {
            dirty_state.depth_bounds_test_enabled = false;
            if (depth_bounds_supported) {
                emit([v = depth_bounds_test_enabled](vk::CommandBuffer c) {
                    c.setDepthBoundsTestEnable(v);
                });
            }
        }
        if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
            dirty_state.depth_bounds = false;
            if (depth_bounds_supported) {
                emit([lo = depth_bounds_min, hi = depth_bounds_max](vk::CommandBuffer c) {
                    c.setDepthBounds(lo, hi);
                });
            }
        }
        if (dirty_state.depth_bias_enabled) {
            dirty_state.depth_bias_enabled = false;
            emit([v = depth_bias_enabled](vk::CommandBuffer c) { c.setDepthBiasEnable(v); });
        }
        if (depth_bias_enabled && dirty_state.depth_bias) {
            dirty_state.depth_bias = false;
            emit([k = depth_bias_constant, clamp = depth_bias_clamp,
                  slope = depth_bias_slope](vk::CommandBuffer c) { c.setDepthBias(k, clamp, slope); });
        }
        if (dirty_state.stencil_test_enabled) {
            dirty_state.stencil_test_enabled = false;
            emit([v = stencil_test_enabled](vk::CommandBuffer c) { c.setStencilTestEnable(v); });
        }
        if (stencil_test_enabled) {
            const auto ops = [&](Face face, const StencilOps& o) {
                emit([face, o](vk::CommandBuffer c) {
                    c.setStencilOp(face, o.fail_op, o.pass_op, o.depth_fail_op, o.compare_op);
                });
            };
            if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
                stencil_front_ops == stencil_back_ops) {
                ops(Face::eFrontAndBack, stencil_front_ops);
            } else {
                if (dirty_state.stencil_front_ops) {
                    ops(Face::eFront, stencil_front_ops);
                }
                if (dirty_state.stencil_back_ops) {
                    ops(Face::eBack, stencil_back_ops);
                }
            }
            dirty_state.stencil_front_ops = dirty_state.stencil_back_ops = false;

            const auto reference = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilReference(face, v); });
            };
            if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
                stencil_front_reference == stencil_back_reference) {
                reference(Face::eFrontAndBack, stencil_front_reference);
            } else {
                if (dirty_state.stencil_front_reference) {
                    reference(Face::eFront, stencil_front_reference);
                }
                if (dirty_state.stencil_back_reference) {
                    reference(Face::eBack, stencil_back_reference);
                }
            }
            dirty_state.stencil_front_reference = dirty_state.stencil_back_reference = false;

            const auto write_mask = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilWriteMask(face, v); });
            };
            if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
                stencil_front_write_mask == stencil_back_write_mask) {
                write_mask(Face::eFrontAndBack, stencil_front_write_mask);
            } else {
                if (dirty_state.stencil_front_write_mask) {
                    write_mask(Face::eFront, stencil_front_write_mask);
                }
                if (dirty_state.stencil_back_write_mask) {
                    write_mask(Face::eBack, stencil_back_write_mask);
                }
            }
            dirty_state.stencil_front_write_mask = dirty_state.stencil_back_write_mask = false;

            const auto compare_mask = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilCompareMask(face, v); });
            };
            if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
                stencil_front_compare_mask == stencil_back_compare_mask) {
                compare_mask(Face::eFrontAndBack, stencil_front_compare_mask);
            } else {
                if (dirty_state.stencil_front_compare_mask) {
                    compare_mask(Face::eFront, stencil_front_compare_mask);
                }
                if (dirty_state.stencil_back_compare_mask) {
                    compare_mask(Face::eBack, stencil_back_compare_mask);
                }
            }
            dirty_state.stencil_front_compare_mask = dirty_state.stencil_back_compare_mask = false;
        }
        if (dirty_state.primitive_restart_enable) {
            dirty_state.primitive_restart_enable = false;
            emit([v = primitive_restart_enable](vk::CommandBuffer c) {
                c.setPrimitiveRestartEnable(v);
            });
        }
        if (dirty_state.rasterizer_discard_enable) {
            dirty_state.rasterizer_discard_enable = false;
            emit([v = rasterizer_discard_enable](vk::CommandBuffer c) {
                c.setRasterizerDiscardEnable(v);
            });
        }
        if (dirty_state.cull_mode) {
            dirty_state.cull_mode = false;
            emit([v = cull_mode](vk::CommandBuffer c) { c.setCullMode(v); });
        }
        if (dirty_state.front_face) {
            dirty_state.front_face = false;
            emit([v = front_face](vk::CommandBuffer c) { c.setFrontFace(v); });
        }
        if (dirty_state.blend_constants) {
            dirty_state.blend_constants = false;
            emit([v = blend_constants](vk::CommandBuffer c) { c.setBlendConstants(v.data()); });
        }
        if (dirty_state.color_write_masks) {
            dirty_state.color_write_masks = false;
            if (color_write_mask_supported) {
                emit([v = color_write_masks](vk::CommandBuffer c) { c.setColorWriteMaskEXT(0, v); });
            }
        }
        if (dirty_state.line_width) {
            dirty_state.line_width = false;
            emit([v = line_width](vk::CommandBuffer c) { c.setLineWidth(v); });
        }
        if (dirty_state.feedback_loop_enabled && feedback_loop_supported) {
            dirty_state.feedback_loop_enabled = false;
            emit([v = feedback_loop_enabled](vk::CommandBuffer c) {
                c.setAttachmentFeedbackLoopEnableEXT(v ? vk::ImageAspectFlagBits::eColor
                                                       : vk::ImageAspectFlagBits::eNone);
            });
        }
    }

    /// spartanport: true when Commit() would record anything. Flags that Commit() defers (their
    /// test is disabled) do not count.
    [[nodiscard]] bool AnyDirty() const noexcept {
        auto pending = dirty_state;
        if (!depth_test_enabled) {
            pending.depth_compare_op = false;
        }
        if (!depth_bounds_test_enabled) {
            pending.depth_bounds = false;
        }
        if (!depth_bias_enabled) {
            pending.depth_bias = false;
        }
        if (!stencil_test_enabled) {
            pending.stencil_front_ops = pending.stencil_back_ops = false;
            pending.stencil_front_reference = pending.stencil_back_reference = false;
            pending.stencil_front_write_mask = pending.stencil_back_write_mask = false;
            pending.stencil_front_compare_mask = pending.stencil_back_compare_mask = false;
        }
        static constexpr decltype(dirty_state) clean{};
        return std::memcmp(&pending, &clean, sizeof(pending)) != 0;
    }

    /// Invalidates all dynamic state to be flushed into the next command buffer.
    void Invalidate() {
        // spartanport: named flags only; padding bits set by a memset made AnyDirty() always true.
        dirty_state.viewports = true;
        dirty_state.scissors = true;
        dirty_state.depth_test_enabled = true;
        dirty_state.depth_write_enabled = true;
        dirty_state.depth_compare_op = true;
        dirty_state.depth_bounds_test_enabled = true;
        dirty_state.depth_bounds = true;
        dirty_state.depth_bias_enabled = true;
        dirty_state.depth_bias = true;
        dirty_state.stencil_test_enabled = true;
        dirty_state.stencil_front_ops = true;
        dirty_state.stencil_front_reference = true;
        dirty_state.stencil_front_write_mask = true;
        dirty_state.stencil_front_compare_mask = true;
        dirty_state.stencil_back_ops = true;
        dirty_state.stencil_back_reference = true;
        dirty_state.stencil_back_write_mask = true;
        dirty_state.stencil_back_compare_mask = true;
        dirty_state.primitive_restart_enable = true;
        dirty_state.rasterizer_discard_enable = true;
        dirty_state.cull_mode = true;
        dirty_state.front_face = true;
        dirty_state.blend_constants = true;
        dirty_state.color_write_masks = true;
        dirty_state.line_width = true;
        dirty_state.feedback_loop_enabled = true;
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
            depth_bias_slope != slope) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if (blend_constants != blend_constants_) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (!std::ranges::equal(color_write_masks, color_write_masks_)) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }
};

using SubmitFunc = Common::UniqueFunction<void, SubmitInfo&>;

/// spartanport: a block of deferred Vulkan commands. Commands are closures placed in
/// fixed storage (no allocation per command) and run in order on the recording thread.
class RecordChunk {
public:
    static constexpr size_t Capacity = 128 * 1024;

    /// Returns false (and leaves `func` untouched) when the chunk has no room.
    template <typename Func>
    bool Push(Func&& func) {
        using Command = TypedCommand<std::decay_t<Func>>;
        static_assert(sizeof(Command) <= Capacity, "recorded command is too large");
        const size_t offset = (used + alignof(Command) - 1) & ~(alignof(Command) - 1);
        if (offset + sizeof(Command) > Capacity) {
            return false;
        }
        auto* command = new (storage + offset) Command(std::forward<Func>(func));
        if (last) {
            last->next = command;
        } else {
            first = command;
        }
        last = command;
        used = offset + sizeof(Command);
        PrefetchAhead();
        return true;
    }

    /// Raw storage in the chunk for a command's variable-length data; null when full.
    void* Allocate(size_t bytes, size_t align) {
        const size_t offset = (used + align - 1) & ~(align - 1);
        if (offset + bytes > Capacity) {
            return nullptr;
        }
        used = offset + bytes;
        PrefetchAhead();
        return storage + offset;
    }

    void Execute(vk::CommandBuffer cmdbuf) {
        for (CommandBase* command = first; command;) {
            CommandBase* const next = command->next;
            command->Execute(cmdbuf);
            command->~CommandBase();
            command = next;
        }
        first = last = nullptr;
        used = 0;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return first == nullptr;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return used;
    }

private:
    /// The recording thread last read these cache lines: the first write to each has to take
    /// it back from that core. Requesting ownership a few lines ahead overlaps those
    /// transfers instead of stalling on each (RecordPrefetch).
    void PrefetchAhead() const {
        if (!SpartanToggle::Disabled(SpartanToggle::RecordPrefetch)) {
            __builtin_prefetch(storage + used + 384, 1, 3);
            __builtin_prefetch(storage + used + 448, 1, 3);
        }
    }

    struct CommandBase {
        virtual ~CommandBase() = default;
        virtual void Execute(vk::CommandBuffer cmdbuf) = 0;
        CommandBase* next{};
    };
    template <typename Func>
    struct TypedCommand final : CommandBase {
        explicit TypedCommand(Func&& func_) : func{std::move(func_)} {}
        explicit TypedCommand(const Func& func_) : func{func_} {}
        void Execute(vk::CommandBuffer cmdbuf) override {
            func(cmdbuf);
        }
        Func func;
    };

    alignas(64) std::byte storage[Capacity];
    size_t used = 0;
    CommandBase* first{};
    CommandBase* last{};
};

class Scheduler {
public:
    /// `threaded_recording`: Vulkan commands passed to Record() are recorded by a worker thread.
    explicit Scheduler(const Instance& instance, bool threaded_recording = false);
    ~Scheduler();

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush(SubmitInfo& info);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish(std::source_location caller = std::source_location::current());

    /// spartanport: waits for the work submitted so far but leaves the command buffer being
    /// recorded open, so a CommandBuffer() the caller holds stays valid (Finish() submits it:
    /// recording into a submitted buffer crashes AMD's driver).
    void WaitSubmitted(std::source_location caller = std::source_location::current());

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick, std::source_location caller = std::source_location::current(),
              u64 submit_ns = 0);

    /// Attempts to execute operations whose tick the GPU has caught up with.
    void PopPendingOperations();

    /// Starts a new rendering scope with provided state.
    void BeginRendering(const RenderState& new_state);

    /// Ends current rendering scope.
    void EndRendering();

    /// spartanport: SPARTAN_FULL_BARRIERS=1 records a full memory barrier around every render pass and
    /// every compute dispatch, to tell a missing barrier from everything else.
    static bool FullBarriers();
    static void RecordFullBarrier(vk::CommandBuffer cmdbuf);

    /// Sets a function to be called on every scheduler submission.
    void SetSubmitCallback(SubmitFunc&& on_submit) {
        this->on_submit = std::move(on_submit);
    }

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the current command buffer for recording on the calling thread. With threaded
    /// recording this first waits until all commands passed to Record() are recorded, then
    /// records directly (Record() included, so callers may mix both) until the next
    /// KickRecording() or submission.
    [[gnu::noinline]] vk::CommandBuffer CommandBuffer() {
        if (recording_threaded && !direct_mode) {
            SyncRecording();
            direct_mode = true;
            direct_recordings.fetch_add(1, std::memory_order_relaxed);
            TraceDirectRecording(__builtin_return_address(0));
        }
        return current_cmdbuf;
    }

    /// SPARTAN_RECORDER_TRACE=1: prints the most frequent CommandBuffer() callers every 2000 calls.
    static void TraceDirectRecording(void* caller);

    /// spartanport: 录制侧线程归属的诊断作用域（SPARTAN_PRODUCER_CHECK=1 开启，默认关闭）。
    /// 进入时抢占归属：抢不到说明**另一个线程**也在录制侧，报告双方线程 id、线程名与
    /// 入口名。关闭时构造与析构都是空操作（常态路径零开销）。诊断只暴露并发事实，不代表
    /// 并发已被消除。
    class OwnershipScope {
    public:
        OwnershipScope(Scheduler& scheduler_, const char* entry) noexcept
            : scheduler{scheduler_}, entry{entry} {
            if (!Enabled()) {
                return;
            }
            const std::uint32_t self = CurrentThreadId();
            std::uint32_t holder = 0;
            if (scheduler.recording_ownership.Enter(self, holder)) {
                acquired = true;
                return;
            }
            // 持有者就是本线程：嵌套进入（例如 Record() 内部再调 RecordData()），不是并发。
            if (holder != self) {
                ReportConcurrentRecording(holder, entry);
                concurrent = true;
            }
        }
        ~OwnershipScope() noexcept {
            if (!Enabled() || !acquired) {
                return;
            }
            scheduler.recording_ownership.Leave();
        }
        OwnershipScope(const OwnershipScope&) = delete;
        OwnershipScope& operator=(const OwnershipScope&) = delete;

        /// 诊断是否开启（进程内只读一次）。
        [[nodiscard]] static bool Enabled() noexcept {
            static const bool enabled = [] {
                const char* value = std::getenv("SPARTAN_PRODUCER_CHECK");
                return value && value[0] == '1';
            }();
            return enabled;
        }

        /// 本次进入是否撞上了另一个线程（诊断用）。
        [[nodiscard]] bool Concurrent() const noexcept {
            return concurrent;
        }

    private:
        Scheduler& scheduler;
        const char* entry;
        bool acquired{false};
        bool concurrent{false};
    };

    /// spartanport: the current recording chunk, never null. The chunk that replaces it is
    /// swapped in before the old one leaves (RetireChunk), so the pointer is always valid; the
    /// null check is the last resort for a handover that raced with us: it hands out a fresh
    /// chunk instead of a null dereference (a null record_chunk crashed SmallGuestCopy with a
    /// read at address 0x20000).
    RecordChunk& CurrentChunk() {
        return chunk_holder.Current();
    }

    /// spartanport: moves the full current chunk aside; the replacement takes its place by
    /// swap, so the current chunk is never empty in between (the old "move out, then install
    /// the new one" left a window where record_chunk was null).
    void RetireChunk() {
        full_chunks.push_back(chunk_holder.Retire());
    }

    /// Records `func(vk::CommandBuffer)` in order with other commands. The closure must own
    /// everything it uses (capture by value): it may run later on the recording thread.
    template <typename Func>
    void Record(Func&& func) {
        if (!recording_threaded || direct_mode) {
            func(current_cmdbuf);
            return;
        }
        if (SpartanToggle::Disabled(SpartanToggle::ThreadedRecording)) {
            SyncRecording();
            direct_mode = true;
            func(current_cmdbuf);
            return;
        }
        // spartanport: 录制侧归属观测（诊断开启时报告两个线程同时在此）。
        const OwnershipScope ownership{*this, "Record"};
        if (!CurrentChunk().Push(std::forward<Func>(func))) {
            RetireChunk();
            const bool pushed = CurrentChunk().Push(std::forward<Func>(func));
            ASSERT(pushed);
        }
    }

    /// True when Record() defers commands (and RecordData() copies into chunks).
    [[nodiscard]] bool IsRecordingDeferred() const noexcept {
        return recording_threaded && !direct_mode &&
               !SpartanToggle::Disabled(SpartanToggle::ThreadedRecording);
    }

    /// Makes room for `bytes` of RecordData() plus the command that uses them in the current
    /// chunk: data and command must share a chunk, which is recycled once executed.
    void ReserveRecordData(size_t bytes) {
        if (!IsRecordingDeferred()) {
            return;
        }
        ASSERT(bytes + 1024 <= RecordChunk::Capacity);
        if (RecordChunk::Capacity - CurrentChunk().Size() < bytes + 1024) {
            RetireChunk();
        }
    }

    /// Copies `data` into recording storage that lives until the command that uses it has
    /// been recorded (the same chunk). With threaded recording off, returns `data` itself.
    template <typename T>
    std::span<const T> RecordData(std::span<const T> data) {
        if (!IsRecordingDeferred() || data.empty()) {
            return data;
        }
        // Room for the data and the command that follows it, so both stay in one chunk.
        const size_t bytes = data.size_bytes();
        ReserveRecordData(bytes + alignof(T));
        auto* dst = static_cast<T*>(CurrentChunk().Allocate(bytes, alignof(T)));
        std::memcpy(dst, data.data(), bytes);
        return {dst, data.size()};
    }

    /// Hands recorded commands to the recording thread. Called at points where no caller holds
    /// the raw command buffer (end of draws and dispatches); small batches are kept.
    void KickRecording(bool force = false);

    /// Waits until every recorded command is in the command buffer.
    void SyncRecording();

    /// spartanport: guest memory copies on the recording thread (small ones, RecordHostCopy) or the
    /// copy threads (SpartanCopy::Async) must be done before the guest learns the GPU is past them
    /// (it may then rewrite the memory, e.g. UI vertices: flickering) and before the
    /// submission that reads them. Waits for all.
    void WaitHostCopies();

    /// Runs `copy` on the recording thread in order with the commands (the thread spins for
    /// work anyway, so small copies cost no wakeup); WaitHostCopies() covers it.
    template <typename Func>
    void RecordHostCopy(Func&& copy) {
        Record([copy = std::forward<Func>(copy), this,
                seq = ++host_copies_issued](vk::CommandBuffer) {
            copy();
            host_copies_done.store(seq, std::memory_order_release);
        });
    }

    /// spartanport: runs `signal` once the guest memory copies issued so far are done, without
    /// waiting here: the recording thread reaches it after the copies queued before it and hands
    /// it to the copy threads' completion (SpartanCopy::AfterCopies).
    void SignalAfterHostCopies(std::function<void()> signal);

    /// spartanport: before a guest-visible write that is not deferred (WriteData, end-of-shader
    /// fences, flip): waits until every signal handed to SignalAfterHostCopies ran, so that
    /// the write cannot overtake an earlier fence. Otherwise the guest, seeing the later value,
    /// may free memory the earlier fence then writes into (corrupted heap, guest fault).
    void WaitDeferredSignals();

    /// Whether a render pass with exactly this state is open.
    [[nodiscard]] bool IsRenderingWith(const RenderState& state) const {
        return is_rendering && render_state == state;
    }

    /// Whether BeginRendering(state) would start a new render pass.
    [[nodiscard]] bool WillBeginRendering(const RenderState& state) const {
        return !(is_rendering && render_state == state);
    }

    /// CommandBuffer() calls that waited for a recording thread (SPARTAN_FRAME_STATS).
    static inline std::atomic<u64> direct_recordings{0};

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return work_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (work_semaphore.IsFree(tick)) {
            return true;
        }
        work_semaphore.Refresh();
        return work_semaphore.IsFree(tick);
    }

    /// Returns the scheduler timeline semaphore.
    [[nodiscard]] Semaphore* GetWorkSemaphore() noexcept {
        return &work_semaphore;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
        pending_ops.emplace(std::move(func), CurrentTick());
        num_pending_ops.fetch_add(1, std::memory_order_release);
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
        }
        priority_pending_ops_cv.notify_one();
    }

    static std::mutex submit_mutex;

private:
    void AllocateWorkerCommandBuffers();

    std::unique_ptr<RecordChunk> AcquireChunk();

    void RecorderThread(std::stop_token stoken);

    void SubmitExecution(SubmitInfo& info);

    void PriorityPendingOpsThread(std::stop_token stoken);

    /// spartanport: 当前线程 id（诊断用；诊断关闭时不会被调用）。
    static std::uint32_t CurrentThreadId();

    /// spartanport: 报告"两个线程同时在录制侧"：双方线程 id、线程名与入口名。输出表明
    /// 并发事实仍然存在，不代表并发已被消除。报告条数有上限。
    static void ReportConcurrentRecording(std::uint32_t holder_tid, const char* entry);

private:
    const Instance& instance;
    Semaphore work_semaphore;
    CommandPool command_pool;
    DynamicState dynamic_state;
    SubmitFunc on_submit{};
    vk::CommandBuffer current_cmdbuf;
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    u32 pending_polls = 0; // spartanport
    std::atomic<u32> num_pending_ops{0}; ///< spartanport: pending_ops.size(), checked without the lock
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
    RenderState render_state;
    bool is_rendering = false;
    // 配对护栏：按 command buffer 代计数的渲染深度（生产路径复用 vk_render_pass_pairing.h，
    // 宿主行为测试用同一实现）。BeginRendering 递增、EndRendering 递减，换代时归零。EndRendering
    // 在深度为 0 时说明记录侧没有对应开始命令（跨代错位），此时告警并跳过该命令，不让驱动解引用
    // 空 subpass。深度只统计"确实会录制的开始命令"，同 state 续接不重复递增；并发下是保守下界。
    RenderPassPairing render_pass_pairing;
    // spartanport: 录制侧线程归属（诊断用，默认关闭）。录制侧设计上由一个线程独占，但 GPU
    // 命令线程与绘制录制线程在交接窗口可能同时进入：本次真机崩溃（fault address 0x20000）
    // 即绘制录制线程在 Record() 里读到已被交接置空的 chunk。此处只把并发事实变成可归因的
    // 证据（双方线程 id 与入口名），不替代对 chunk 的保护——chunk 由 swap 语义保证永不为空。
    RecordingOwnership recording_ownership;
    // spartanport: threaded recording
    // spartanport: 当前录制 chunk 的持有者（swap 语义，纯逻辑组件，生产路径与宿主行为测试
    // 共用同一实现，见 vk_recording_chunk_holder.h）。当前 chunk 永不为空：交接时先取好
    // 替换者再交换指针，"当前 chunk 为空"的中间态不可表达。
    RecordingChunkHolder<RecordChunk> chunk_holder{[this] { return AcquireChunk(); }};
    std::vector<std::unique_ptr<RecordChunk>> full_chunks;
    std::mutex recorder_mutex;
    std::condition_variable_any recorder_cv;
    std::condition_variable_any recorder_idle_cv;
    std::deque<std::unique_ptr<RecordChunk>> recorder_queue;
    std::vector<std::unique_ptr<RecordChunk>> free_chunks;
    bool recorder_busy = false;
    std::atomic<size_t> queued_chunks{0}; ///< recorder_queue.size() for lock-free polling
    bool recorder_sleeping = false; ///< waiting on recorder_cv (guarded by recorder_mutex)
    bool direct_mode = false; ///< the command buffer is recorded on the caller's thread
    u64 host_copies_issued = 0;
    std::atomic<u64> host_copies_done{0};
    std::atomic<u64> deferred_signals_issued{0}; ///< by the thread recording (A or B)
    std::shared_ptr<std::atomic<u64>> deferred_signals_done =
        std::make_shared<std::atomic<u64>>(0);
    // spartanport: whether recorder_thread runs, read on every Record(). std::jthread::joinable()
    // compares thread ids, which on Windows (libc++ over winpthreads) costs two system calls.
    bool recording_threaded{false};
    std::jthread recorder_thread;
    tracy::VkCtxScope* profiler_scope{};
};

} // namespace Vulkan
