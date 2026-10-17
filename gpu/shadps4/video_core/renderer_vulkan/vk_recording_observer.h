// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_recording_diagnostics.h"

namespace Vulkan {

inline uintptr_t RecordingBufferId(vk::CommandBuffer buffer) {
    return reinterpret_cast<uintptr_t>(static_cast<VkCommandBuffer>(buffer));
}

inline RecordingDiagnostics::Scope ObserveRecording(vk::CommandBuffer buffer, const char* entry) {
    auto& tracker = RecordingDiagnostics::Get();
    return {tracker, RecordingDiagnostics::Kind::Recording,
            tracker.Sequence(RecordingBufferId(buffer)), entry};
}

// 保留 vk::CommandBuffer 的调用接口，但借用期覆盖返回句柄之后的实际录制。
// 不可复制，避免把观察范围静默延长或提前释放；调用点用 const auto 保持此对象存活。
class ObservedCommandBuffer : public vk::CommandBuffer {
public:
    ObservedCommandBuffer(vk::CommandBuffer buffer, const char* entry)
        : vk::CommandBuffer{buffer}, observation{ObserveRecording(buffer, entry)} {}
    ObservedCommandBuffer(ObservedCommandBuffer&&) = default;
    ObservedCommandBuffer(const ObservedCommandBuffer&) = delete;
private:
    RecordingDiagnostics::Scope observation;
};

inline void ObserveRenderBegin(vk::CommandBuffer buffer, const char* entry) {
    RecordingDiagnostics::Get().Rendering(RecordingBufferId(buffer), true, entry);
}

inline void ObserveRenderEnd(vk::CommandBuffer buffer, const char* entry) {
    RecordingDiagnostics::Get().Rendering(RecordingBufferId(buffer), false, entry);
}

} // namespace Vulkan
