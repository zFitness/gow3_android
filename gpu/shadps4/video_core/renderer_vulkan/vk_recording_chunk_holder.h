// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace Vulkan {

/// 当前录制 chunk 的持有者：swap 语义的纯逻辑组件。
/// Scheduler 与宿主行为测试共用同一实现：不依赖 Vulkan，只管理"当前 chunk 是谁"。
///
/// 语义：
///  - Current() 返回的引用**永不为空**：观察到空时用 acquire 补齐后再返回，绝不把空
///    指针交给调用方（录制入口无锁解引用空 chunk 就是本次真机崩溃：fault address
///    0x20000，SmallGuestCopy）。
///  - 交接用 swap：Retire() / Handover() 都先取好替换者，再与当前者交换指针，
///    "当前 chunk 为空"的中间态在结构上不可表达。旧的"先 move 走、之后补装"留了空窗，
///    绘制录制线程恰好落在窗口里就解引用到空 chunk。
///  - 让位的旧 chunk 由调用方接管（移入待交接队列或交给录制线程）：命令不丢失、
///    不重复、顺序不变。
///
/// 并发约定：
///  - 当前 chunk 的**读取**（Current()）只读一个原子指针，不取锁：热路径没有锁开销，
///    也不会与"更换"产生数据竞争（裸 unique_ptr 成员在并发 swap 下是未定义行为）。
///  - 当前 chunk 的**更换**（Retire() / Handover()）取锁，保证"取好替换者"与"交换指针"
///    是一个整体。
///  - 所有权：owned_ 始终是 current_ 所指 chunk 的所有者（只在锁下更换）。让位时所有权
///    交给调用方，调用方把它放进队列——队列持有期间，读者手里的指针始终有效。
///  - 锁序：唯一的一处跨锁顺序是"先取 recorder_mutex（acquire_）、再取本组件的锁"。
///    调用方**不得**持有 recorder_mutex 时调用 Retire()（锁序反转）；Handover() 不取新
///    chunk，因此在 recorder_mutex 内调用是安全的。
///
/// 归属诊断（另一个线程也在录制侧）与本组件无关，见 vk_recording_ownership.h：
/// 本组件保证"不崩"，诊断只把并发事实变成可归因的证据。
template <typename Chunk>
class RecordingChunkHolder {
public:
    /// 取一个新 chunk 的方式（Scheduler::AcquireChunk 自带 recorder_mutex 锁）。
    using AcquireFn = std::function<std::unique_ptr<Chunk>()>;

    explicit RecordingChunkHolder(AcquireFn acquire) : acquire_{std::move(acquire)} {}

    /// 当前 chunk：永不为空。空则当场补齐（[[unlikely]]：补齐是兜底，不是常态）。
    /// 读到的是**发布**出去的指针：先发布指针、再转移所有权，读者看到的指针在任何
    /// 时刻都指向一个活的对象（让位的 chunk 由调用方接管，不会被就地销毁）。
    Chunk& Current() {
        Chunk* chunk = current_.load(std::memory_order_acquire);
        if (!chunk) [[unlikely]] {
            chunk = Fill();
        }
        return *chunk;
    }

    /// 换 chunk（当前 chunk 已满）：先取好替换者，再与当前者交换指针，让位的旧 chunk
    /// 交回调用方。当前 chunk 在整个过程中始终有效——交换的是指针，不是"先清空再装填"。
    /// 返回的旧 chunk 在"此前从未补齐过"时为空（调用方须判空）。
    std::unique_ptr<Chunk> Retire() {
        // 先取好替换者（acquire_ 会取 recorder_mutex），再进锁交换：跨锁顺序单一。
        auto next = acquire_();
        std::scoped_lock lk{mutex_};
        current_.exchange(next.get(), std::memory_order_acq_rel);
        // owned_ 一直是 current_ 所指 chunk 的所有者：把旧的所有权交出去，新的接管。
        std::unique_ptr<Chunk> retired = std::move(owned_);
        owned_ = std::move(next);
        return retired;
    }

    /// 交接：换上 replacement，把让位的旧 chunk 交回调用方。
    /// replacement 为空时不做任何事（当前 chunk 保持原样），返回空。
    /// 注意：本操作**不取新 chunk**，因此可以在持有 Scheduler::recorder_mutex 时调用
    /// （AcquireChunk 自抢该锁，持锁时调用会自锁）。
    std::unique_ptr<Chunk> Handover(std::unique_ptr<Chunk> replacement) {
        if (!replacement) {
            return nullptr;
        }
        std::scoped_lock lk{mutex_};
        current_.exchange(replacement.get(), std::memory_order_acq_rel);
        std::unique_ptr<Chunk> retired = std::move(owned_);
        owned_ = std::move(replacement);
        return retired;
    }

private:
    /// 补齐当前 chunk（冷路径）。取锁，保证只有一个线程在补。
    Chunk* Fill() {
        // 先取好新 chunk，再进锁：跨锁顺序单一（不持本组件的锁时取 recorder_mutex）。
        auto fresh = acquire_();
        std::scoped_lock lk{mutex_};
        Chunk* chunk = current_.load(std::memory_order_acquire);
        if (!chunk) [[unlikely]] {
            owned_ = std::move(fresh);
            chunk = owned_.get();
            current_.store(chunk, std::memory_order_release);
        }
        return chunk;
    }

    AcquireFn acquire_;
    /// 当前 chunk 的**发布**指针（读者只看它，不取锁）。
    std::atomic<Chunk*> current_{nullptr};
    /// current_ 所指 chunk 的所有权（只在 mutex_ 下更换）。
    std::unique_ptr<Chunk> owned_;
    /// 只保护"更换 / 补齐"，不保护读取。
    std::mutex mutex_;
};

} // namespace Vulkan
