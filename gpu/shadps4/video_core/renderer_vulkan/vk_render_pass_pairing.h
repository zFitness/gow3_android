// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace Vulkan {

/// 渲染范围配对记账的纯逻辑组件（移植自 bionic 线 `569b2d43`）。
/// Scheduler 与宿主行为测试共用同一实现：不依赖 Vulkan，深度只记"记录侧意图"。
///
/// 语义：
///  - 任一 command buffer 代内，开始与结束配对；结束不得出现在没有对应开始的代（跨代错位）。
///  - Swap 在换代时调用，把深度归 0：若开始落在旧代而孤立结束落在新代，OnEnd 返回 false，
///    调用方应告警并跳过该结束命令（护栏），而不是把无开始的命令交给驱动
///    （Turnip 会解引用空 subpass，fault 0x4e）。
///  - 深度是并发下的保守下界：只拦确定非法的结束命令，不误伤合法配对。
class RenderPassPairing {
public:
    /// 换代：新 command buffer 代深度归 0。
    void Swap() {
        depth.store(0, std::memory_order_relaxed);
    }

    /// 开始命令被录制：递增深度，返回新深度（>0 表示当前代有打开的渲染范围）。
    std::uint32_t OnBegin() {
        return depth.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    /// 结束请求：若当前代深度为 0（没有对应开始，跨代错位）返回 false，调用方应告警并跳过该
    /// 结束命令；否则递减并返回 true，调用方照常录制结束命令。
    bool OnEnd() {
        if (depth.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        depth.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    /// 当前代深度。
    [[nodiscard]] std::uint32_t Depth() const {
        return depth.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint32_t> depth{0};
};

} // namespace Vulkan
