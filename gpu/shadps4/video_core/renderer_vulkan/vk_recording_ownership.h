// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace Vulkan {

/// 录制侧线程归属的纯逻辑组件。Scheduler 与宿主行为测试共用同一实现：不依赖 Vulkan，
/// 只回答"此刻录制侧是不是只有我"与"刚才是谁"。
///
/// 语义：
///  - 录制侧（当前录制 chunk 的读写）设计上应由一个线程独占：GPU 命令线程把绘制交给
///    绘制录制线程后，两者在交接窗口可能同时触碰录制状态（本次真机崩溃即此类：绘制录制
///    线程在 Record() 里读到已被交接置空的 chunk，fault address 0x20000）。
///  - Enter 抢占归属：抢到即独占；抢不到说明**另一个线程**也在录制侧，此时由调用方
///    报告双方线程身份（诊断默认关闭，见 SPARTAN_PRODUCER_CHECK）。
///  - 归属是"诊断用的独占标记"，不是同步原语：它不替代对 chunk 的保护，只把并发事实
///    变成可归因的证据。chunk 本身由 swap 语义保证永不为空（见 Scheduler::CurrentChunk）。
///
/// 实现要点：归属用一次 CAS 抢占（0 = 无人持有，否则为持有者线程 id），因此"两个线程
/// 同时在录制侧"会被观察到——CAS 失败时，失败方的 expected 值就是持有者线程 id。
class RecordingOwnership {
public:
    /// 进入录制侧。tid 为调用线程 id。返回 true 表示抢到了独占（调用方负责在离开时
    /// Leave）；false 表示已有别的线程持有，此时 holder_tid 为持有者线程 id。
    /// 注意：持有者可能就是本线程（嵌套进入，例如 Record() 内部再调 RecordData()）——
    /// 那不是并发，调用方需自行比对 tid 后再决定是否报告。
    bool Enter(std::uint32_t tid, std::uint32_t& holder_tid) noexcept {
        std::uint32_t expected = 0;
        if (holder_.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
            return true;
        }
        holder_tid = expected;
        return false;
    }

    /// 离开录制侧：释放独占。只有真正抢到独占的那一层才该调用。
    void Leave() noexcept {
        holder_.store(0, std::memory_order_release);
    }

    /// 当前持有者线程 id（0 = 无人持有）。
    [[nodiscard]] std::uint32_t Holder() const noexcept {
        return holder_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::uint32_t> holder_{0}; ///< 0 = 无人持有；否则为持有者线程 id
};

} // namespace Vulkan
