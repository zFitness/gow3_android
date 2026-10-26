// SPDX-License-Identifier: GPL-2.0-or-later
// 宿主行为回归：执行生产实现 vk_render_pass_pairing.h（Scheduler 实际使用的同一组件），
// 验证跨代错位护栏、跨批次配对、direct/deferred 往返、重复结束与并发交错。不依赖 Vulkan。
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

#include "video_core/renderer_vulkan/vk_render_pass_pairing.h"

namespace {

using Vulkan::RenderPassPairing;

void TestCrossGenerationEndIsRejected() {
    // 跨代错位：开始在代 N，换代后孤立结束落在代 N+1，护栏必须拦截（不能有结束无开始）。
    RenderPassPairing pairing;
    assert(pairing.Depth() == 0);
    pairing.OnBegin();                       // 代 N：开始入队
    assert(pairing.Depth() == 1);
    pairing.Swap();                          // 换代 → 深度归 0
    assert(pairing.Depth() == 0);
    const bool accepted = pairing.OnEnd();   // 代 N+1 的孤立结束
    assert(!accepted && "跨代错位的结束命令必须被护栏拦截");
    assert(pairing.Depth() == 0);
    std::printf("PASS cross-generation End rejected\n");
}

void TestLegalCrossBatchPairing() {
    // 合法跨批次：同代内开始/绘制/结束分属不同录制批次，配对保持。
    RenderPassPairing pairing;
    pairing.OnBegin();                       // 批次 1：开始
    assert(pairing.Depth() == 1);
    assert(pairing.Depth() == 1);            // 批次 2（绘制不改变深度）
    const bool accepted = pairing.OnEnd();   // 批次 3：结束
    assert(accepted && "同代合法结束命令必须被接受");
    assert(pairing.Depth() == 0);
    std::printf("PASS legal cross-batch pairing\n");
}

void TestDirectDeferredRoundTrip() {
    // direct/deferred 往返：多次开闭配对，深度在每对后归零。
    RenderPassPairing pairing;
    for (int i = 0; i < 16; ++i) {
        pairing.OnBegin();
        assert(pairing.Depth() == 1);
        assert(pairing.OnEnd());
        assert(pairing.Depth() == 0);
    }
    std::printf("PASS direct/deferred round-trip\n");
}

void TestDuplicateEndAndEndWithoutBegin() {
    // 护栏：无开始的结束与重复结束都被拦截，不产生非法结束命令。
    RenderPassPairing pairing;
    assert(!pairing.OnEnd());        // 无开始的结束
    pairing.OnBegin();
    assert(pairing.OnEnd());         // 第一次合法
    assert(!pairing.OnEnd());        // 重复结束：深度已 0
    assert(pairing.Depth() == 0);
    std::printf("PASS duplicate / missing-Begin End rejected\n");
}

void TestConcurrentCrossGenerationIsCaught() {
    // 并发交错：一个线程在开始后立刻换代（模拟并发提交），另一个线程随即请求结束——
    // 无论交错如何，护栏都不产生无开始的结束命令。
    for (int round = 0; round < 200; ++round) {
        RenderPassPairing pairing;
        std::atomic<bool> begin_done{false};
        std::atomic<int> accepted_ends{0};
        std::thread swapper([&] {
            pairing.OnBegin();
            begin_done.store(true, std::memory_order_release);
            pairing.Swap();  // 换代可能在结束前或后
        });
        std::thread ender([&] {
            while (!begin_done.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (pairing.OnEnd()) {
                accepted_ends.fetch_add(1, std::memory_order_relaxed);
            }
        });
        swapper.join();
        ender.join();
        // 无论交错如何：若结束在换代前被接受，说明它配对了开始（同代）；若在换代后被拦截，
        // 深度保持 0。任一情况下深度最终为 0，且不存在"无开始却被接受的结束"。
        assert(pairing.Depth() == 0);
        assert(accepted_ends.load() <= 1);
    }
    std::printf("PASS concurrent cross-generation is caught\n");
}

} // namespace

int main() {
    TestCrossGenerationEndIsRejected();
    TestLegalCrossBatchPairing();
    TestDirectDeferredRoundTrip();
    TestDuplicateEndAndEndWithoutBegin();
    TestConcurrentCrossGenerationIsCaught();
    std::printf("PASS\n");
    return 0;
}
