// SPDX-License-Identifier: GPL-2.0-or-later
// 宿主行为回归：执行生产实现 vk_recording_chunk_holder.h 与 vk_recording_ownership.h
// （Scheduler 实际使用的同一组件），验证：
//   - 交接期间当前 chunk 始终有效（永不为空，解引用安全）；
//   - 换 chunk 后命令不丢失、不重复、顺序不变；
//   - 录制侧归属的抢占/释放、嵌套进入不是并发、并发进入被观察到。
// 不依赖 Vulkan。
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "video_core/renderer_vulkan/vk_recording_chunk_holder.h"
#include "video_core/renderer_vulkan/vk_recording_ownership.h"

namespace {

using Vulkan::RecordingChunkHolder;
using Vulkan::RecordingOwnership;

/// 假的录制 chunk：记录被追加的命令序列，用于验证命令不丢失/不重复/顺序不变。
/// 追加是线程安全的（生产实现里命令由录制线程追加、录制线程执行；这里让测试能并发追加）。
class FakeChunk {
public:
    void Push(int command) {
        std::scoped_lock lk{mutex};
        commands.push_back(command);
    }
    [[nodiscard]] bool Empty() const {
        std::scoped_lock lk{mutex};
        return commands.empty();
    }
    [[nodiscard]] std::vector<int> Commands() const {
        std::scoped_lock lk{mutex};
        return commands;
    }
    [[nodiscard]] size_t Count() const {
        std::scoped_lock lk{mutex};
        return commands.size();
    }

private:
    mutable std::mutex mutex;
    std::vector<int> commands;
};

/// chunk 来源：每次分配一个新 chunk。分配出去的 chunk 由调用方持有（生产实现里
/// 让位的 chunk 进入待交接队列或录制队列，不会被立刻销毁）。
struct ChunkSource {
    std::atomic<int> acquire_count{0};

    std::unique_ptr<FakeChunk> Acquire() {
        acquire_count.fetch_add(1, std::memory_order_relaxed);
        return std::make_unique<FakeChunk>();
    }
};

// ---------------------------------------------------------------------------
// 当前 chunk 永不为空
// ---------------------------------------------------------------------------

void TestCurrentChunkIsNeverNull() {
    ChunkSource source;
    RecordingChunkHolder<FakeChunk> holder{[&] { return source.Acquire(); }};
    // 初始状态：尚未取过 chunk。Current() 必须补齐而不是返回空引用。
    FakeChunk& first = holder.Current();
    assert(first.Empty());
    assert(source.acquire_count.load() == 1 && "Current() 必须补齐一个 chunk");
    // 再次访问：同一个 chunk，不重复分配。
    FakeChunk& again = holder.Current();
    assert(&again == &first && "重复访问必须命中同一个 chunk");
    assert(source.acquire_count.load() == 1 && "命中缓存时不得再分配");
    std::printf("PASS current chunk is never null\n");
}

void TestCurrentChunkSurvivesHandover() {
    ChunkSource source;
    RecordingChunkHolder<FakeChunk> holder{[&] { return source.Acquire(); }};
    FakeChunk& original = holder.Current();
    original.Push(1);

    // 交接：换上替换者，让位的旧 chunk 交回调用方。
    auto replacement = source.Acquire();
    auto handed = holder.Handover(std::move(replacement));
    assert(handed != nullptr && "交接必须交回让位的旧 chunk");
    assert(handed->Count() == 1 && handed->Commands()[0] == 1);
    // 交接后当前 chunk 依然有效（不是空引用），且是新的那个。
    FakeChunk& after = holder.Current();
    assert(&after != handed.get() && "交接后当前 chunk 必须是替换者");
    std::printf("PASS current chunk survives handover\n");
}

void TestEmptyReplacementLeavesCurrentAlone() {
    ChunkSource source;
    RecordingChunkHolder<FakeChunk> holder{[&] { return source.Acquire(); }};
    FakeChunk& original = holder.Current();
    original.Push(7);

    // 交接一个空替换者：当前 chunk 不变（交接空 chunk 没有意义）。
    auto handed = holder.Handover(nullptr);
    assert(handed == nullptr && "空替换者不得交回任何 chunk");
    FakeChunk& after = holder.Current();
    assert(&after == &original && "空替换者不得换掉当前 chunk");
    assert(after.Commands()[0] == 7 && "当前 chunk 的命令必须原样保留");
    std::printf("PASS empty replacement leaves current chunk alone\n");
}

// ---------------------------------------------------------------------------
// 换 chunk 后命令不丢失、不重复、顺序不变
// ---------------------------------------------------------------------------

void TestRetireKeepsCommandOrder() {
    ChunkSource source;
    RecordingChunkHolder<FakeChunk> holder{[&] { return source.Acquire(); }};

    // 第一段 chunk：追加 1..5。
    for (int i = 1; i <= 5; ++i) {
        holder.Current().Push(i);
    }
    // 换 chunk：旧 chunk 让位。
    auto retired = holder.Retire();
    assert(retired != nullptr && "换 chunk 必须交回旧 chunk");
    const std::vector<int> first = retired->Commands();
    assert(first.size() == 5);
    for (std::size_t i = 0; i < first.size(); ++i) {
        assert(first[i] == static_cast<int>(i) + 1 && "旧 chunk 的命令顺序必须不变");
    }
    // 第二段 chunk：追加 6..8。命令不因换 chunk 而重复或错序。
    for (int i = 6; i <= 8; ++i) {
        holder.Current().Push(i);
    }
    auto retired2 = holder.Retire();
    const std::vector<int> second = retired2->Commands();
    assert(second.size() == 3);
    for (std::size_t i = 0; i < second.size(); ++i) {
        assert(second[i] == static_cast<int>(i) + 6);
    }
    // 两段拼起来正好是 1..8：不丢失、不重复。
    std::vector<int> all = first;
    all.insert(all.end(), second.begin(), second.end());
    assert(all.size() == 8);
    for (std::size_t i = 0; i < all.size(); ++i) {
        assert(all[i] == static_cast<int>(i) + 1 && "拼接后的命令序列必须是 1..8");
    }
    std::printf("PASS retire keeps command order\n");
}

void TestCommandsSurviveManyHandoffs() {
    ChunkSource source;
    RecordingChunkHolder<FakeChunk> holder{[&] { return source.Acquire(); }};

    // 反复交接：每轮往当前 chunk 追加若干命令，然后把让位的 chunk 收起来。
    std::vector<std::unique_ptr<FakeChunk>> handed_over;
    int next = 0;
    for (int round = 0; round < 16; ++round) {
        for (int i = 0; i < 4; ++i) {
            holder.Current().Push(++next);
        }
        handed_over.push_back(holder.Retire());
    }
    // 所有让位的 chunk 拼起来必须是 1..64，顺序不变。
    std::vector<int> all;
    for (const auto& chunk : handed_over) {
        const std::vector<int> commands = chunk->Commands();
        all.insert(all.end(), commands.begin(), commands.end());
    }
    assert(all.size() == 64);
    for (std::size_t i = 0; i < all.size(); ++i) {
        assert(all[i] == static_cast<int>(i) + 1 && "命令序列必须严格递增且不丢失");
    }
    std::printf("PASS commands survive many handoffs\n");
}

// ---------------------------------------------------------------------------
// 并发：交接与追加交错时，当前 chunk 始终有效
// ---------------------------------------------------------------------------

void TestConcurrentAppendAndHandoverKeepsCurrentValid() {
    ChunkSource source;
    RecordingChunkHolder<FakeChunk> holder{[&] { return source.Acquire(); }};

    std::atomic<bool> stop{false};
    std::atomic<int> appended{0};

    // 让位的 chunk 全部保留（生产实现里它们进入待交接队列或录制队列，不会被立刻销毁，
    // 因此追加线程手里的引用始终指向活的对象）。
    std::mutex retired_mutex;
    std::vector<std::unique_ptr<FakeChunk>> retired;

    // 追加线程：不停地往当前 chunk 里追加命令。
    std::thread appender([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            // Current() 永不为空：拿到的引用必然指向一个活的 chunk。
            // 若交接期间当前 chunk 变空，这里会解引用空指针（崩溃）而不是静默通过。
            FakeChunk& chunk = holder.Current();
            chunk.Push(appended.fetch_add(1, std::memory_order_relaxed) + 1);
        }
    });

    // 交接线程：不停地换 chunk 并把让位的 chunk 收走。
    std::thread handover([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            auto old = holder.Retire();
            if (old) {
                std::scoped_lock lk{retired_mutex};
                retired.push_back(std::move(old));
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_relaxed);
    appender.join();
    handover.join();

    const int total = appended.load();
    assert(total > 0 && "追加线程必须真的追加了命令");

    // 命令不丢失、不重复：所有让位 chunk 加上当前 chunk 的命令总数等于追加总数。
    size_t counted = 0;
    {
        std::scoped_lock lk{retired_mutex};
        for (const auto& chunk : retired) {
            counted += chunk->Count();
        }
    }
    counted += holder.Current().Count();
    assert(counted == static_cast<size_t>(total) && "并发交接期间命令不得丢失或重复");
    std::printf("PASS concurrent append/handover keeps current chunk valid (appended=%d)\n", total);
}

// ---------------------------------------------------------------------------
// 录制侧归属（诊断）
// ---------------------------------------------------------------------------

void TestSingleOwnerEnterLeave() {
    RecordingOwnership ownership;
    std::uint32_t holder = 0;
    const std::uint32_t self = 1001;
    assert(ownership.Holder() == 0);
    assert(ownership.Enter(self, holder) && "首个进入者必须抢到独占");
    assert(ownership.Holder() == self);
    ownership.Leave();
    assert(ownership.Holder() == 0 && "离开后必须释放归属");
    // 释放后可以再次独占。
    assert(ownership.Enter(self, holder));
    ownership.Leave();
    std::printf("PASS single owner enter/leave\n");
}

void TestNestedEnterIsNotConcurrency() {
    // 嵌套进入（Record() 内部再调 RecordData()）：拿不到独占，但持有者就是本线程，
    // 这不是并发——调用方据此不报告。
    RecordingOwnership ownership;
    std::uint32_t holder = 0;
    const std::uint32_t self = 2002;
    assert(ownership.Enter(self, holder));
    assert(!ownership.Enter(self, holder) && "嵌套进入不得抢到独占");
    assert(holder == self && "嵌套进入时持有者应是本线程");
    ownership.Leave(); // 只有外层释放
    assert(ownership.Holder() == 0);
    std::printf("PASS nested enter is not concurrency\n");
}

void TestConcurrentEnterIsObserved() {
    // 两个线程同时进入：只有一个能抢到，另一个观察到对方并拿到持有者线程 id。
    RecordingOwnership ownership;
    std::atomic<int> owners{0};
    std::atomic<int> conflicts{0};
    std::atomic<std::uint32_t> observed_holder{0};
    std::atomic<bool> go{false};

    const auto worker = [&](std::uint32_t tid) {
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::uint32_t holder = 0;
        if (ownership.Enter(tid, holder)) {
            owners.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            ownership.Leave();
        } else {
            conflicts.fetch_add(1, std::memory_order_relaxed);
            observed_holder.store(holder, std::memory_order_relaxed);
        }
    };

    std::thread a(worker, 3003);
    std::thread b(worker, 4004);
    go.store(true, std::memory_order_release);
    a.join();
    b.join();

    assert(owners.load() == 1 && "同一时刻只允许一个持有者");
    assert(conflicts.load() == 1 && "另一个线程必须观察到冲突");
    const std::uint32_t holder = observed_holder.load();
    assert((holder == 3003 || holder == 4004) && "冲突方必须能给出持有者线程 id");
    std::printf("PASS concurrent enter observed (holder=%u)\n", holder);
}

void TestOwnershipIsReleasedAfterContention() {
    // 竞争之后归属必须回到无人持有：持有者离开后，下一个进入者能拿到独占。
    RecordingOwnership ownership;
    std::uint32_t holder = 0;
    const std::uint32_t self = 5005;
    assert(ownership.Enter(self, holder));
    ownership.Leave();
    assert(ownership.Holder() == 0);
    std::uint32_t other = 0;
    assert(ownership.Enter(6006, other));
    assert(ownership.Holder() == 6006);
    ownership.Leave();
    std::printf("PASS ownership released after contention\n");
}

} // namespace

int main() {
    TestCurrentChunkIsNeverNull();
    TestCurrentChunkSurvivesHandover();
    TestEmptyReplacementLeavesCurrentAlone();
    TestRetireKeepsCommandOrder();
    TestCommandsSurviveManyHandoffs();
    TestConcurrentAppendAndHandoverKeepsCurrentValid();
    TestSingleOwnerEnterLeave();
    TestNestedEnterIsNotConcurrency();
    TestConcurrentEnterIsObserved();
    TestOwnershipIsReleasedAfterContention();
    std::printf("PASS all recording chunk ownership tests\n");
    return 0;
}
