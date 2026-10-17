// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_recording_diagnostics.h"

#include <cassert>
#include <future>
#include <iostream>

using namespace Vulkan::RecordingDiagnostics;

static void Overlap(Tracker& tracker, Kind kind, Key first, Key second,
                    const char* a, const char* b) {
    std::promise<void> entered, finished;
    auto ready = entered.get_future();
    auto done = finished.get_future();
    std::thread worker([&] {
#ifdef __APPLE__
        pthread_setname_np("VkRecorder-test");
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), "VkRecorder-test");
#endif
        Scope outer{tracker, kind, first, a};
        // 同线程嵌套不能提前释放外层观察范围。
        { Scope nested{tracker, kind, first, "nested"}; }
        entered.set_value();
        done.wait();
    });
    ready.wait();
    { Scope scope{tracker, kind, second, b}; }
    finished.set_value();
    worker.join();
}

int main() {
    std::vector<Event> events;
    Tracker tracker{{true, true, true, 8}, [&](const Event& e) { events.push_back(e); }};
    const Key key = tracker.NewSequence(0x1234);
    {
        Scope outer{tracker, Kind::Recording, key, "direct"};
        Scope inner{tracker, Kind::Recording, key, "immediate"};
    }
    assert(events.empty());

    Overlap(tracker, Kind::Producer, {0x888, 0}, {0x888, 0}, "RecordData", "KickRecording");
    assert(events.size() == 2 && events[0].kind == Kind::Producer);
    assert(!events[0].completed && events[1].completed);
    assert(events[0].first.entry == "RecordData" && events[0].second.entry == "KickRecording");
    assert(events[0].first.thread != events[0].second.thread);
    assert(events[0].first.name == "VkRecorder-test");
    assert(!events[0].second.name.empty());
    events.clear();

    Overlap(tracker, Kind::Recording, key, key, "VkRecorder::Execute", "direct");
    assert(events.size() == 2 && events[0].kind == Kind::Recording);
    const auto& completed = events[1];
    assert(completed.first.entry == "VkRecorder::Execute");
    assert(completed.first.start_ns <= completed.second.start_ns);
    assert(completed.first.end_ns >= completed.second.end_ns);
    assert(completed.first.end_ns && completed.second.end_ns);
    events.clear();

    Overlap(tracker, Kind::Recording, key, key, "direct-A", "direct-B");
    assert(events.size() == 2);
    events.clear();
    Overlap(tracker, Kind::Recording, key, tracker.NewSequence(0x5678), "direct-A", "other-buffer");
    Overlap(tracker, Kind::Recording, key, tracker.NewSequence(0x1234), "old-generation", "new-generation");
    Overlap(tracker, Kind::Producer, {0x888, 0}, {0x999, 0}, "scheduler-A", "scheduler-B");
    Overlap(tracker, Kind::Producer, {0x888, 0}, key, "enqueue", "other-producer");
    assert(events.empty());
    // 正常生产者入队与录制线程执行属于不同检测域。
    {
        Scope producer{tracker, Kind::Producer, key, "enqueue"};
        std::thread consumer([&] { Scope recording{tracker, Kind::Recording, key, "execute"}; });
        consumer.join();
    }
    assert(events.empty());

    tracker.NewSequence(0x99);
    tracker.Rendering(0x99, true, "begin-chunk-1");
    // 批次交接不改变实际 render pass 状态。
    tracker.Rendering(0x99, false, "end-chunk-2");
    tracker.EndSequence(0x99);
    assert(events.empty());
    tracker.Rendering(0x99, false, "duplicate-end");
    assert(events.size() == 1 && events.back().kind == Kind::Pairing);
    assert(std::string(events.back().reason) == "end-without-begin" && events.back().sequence == 3);
    tracker.Rendering(0x99, true, "begin");
    tracker.Rendering(0x99, true, "duplicate-begin");
    tracker.EndSequence(0x99);
    assert(events.size() == 3);
    assert(std::string(events.back().reason) == "submit-with-open-renderpass");
    tracker.NewSequence(0x99);
    tracker.Rendering(0x99, false, "no-begin");
    assert(events.back().sequence == 1);
    events.clear();

    for (int i = 0; i < 12; ++i)
        Overlap(tracker, Kind::Recording, key, key, "direct-A", "direct-B");
    // 此 tracker 之前已报告两次实际录制重叠，剩余六条，每条只有 open/closed 两次输出。
    assert(events.size() == 12);
    assert(events.back().report == 8);
    events.clear();
    Tracker disabled{{}, [&](const Event& e) { events.push_back(e); }};
    Overlap(disabled, Kind::Recording, key, key, "A", "B");
    disabled.NewSequence(0x99);
    disabled.Rendering(0x99, false, "end");
    assert(events.empty());
    std::cout << "recording diagnostics: overlap, nesting, generation, pairing, limit, disabled PASS\n";
}
