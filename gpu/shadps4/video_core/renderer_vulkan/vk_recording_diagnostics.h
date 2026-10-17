// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#endif

namespace Vulkan::RecordingDiagnostics {

enum class Kind { Producer, Recording, Pairing };

struct Key {
    uintptr_t buffer{};
    uint64_t generation{};
    bool operator==(const Key&) const = default;
};

struct Interval {
    uint64_t token{}, thread{}, start_ns{}, end_ns{};
    std::string name, entry;
    Key key{};
    Kind kind{};
    unsigned depth{1};
};

struct Event {
    Kind kind{};
    uint64_t report{}, sequence{};
    const char* reason{};
    bool completed{};
    Interval first{}, second{};
};

struct Options {
    bool producer{}, recording{}, pairing{};
    unsigned limit{8};
};

inline uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline uint64_t ThreadId() {
#ifdef __linux__
    return static_cast<uint64_t>(syscall(SYS_gettid));
#elif defined(__APPLE__)
    uint64_t id{};
    pthread_threadid_np(nullptr, &id);
    return id;
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
}

inline std::string ThreadName() {
    char name[64]{};
#ifndef _WIN32
    pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
    return name[0] ? name : "unnamed";
}

inline void Print(const Event& event) {
    const char* kind = event.kind == Kind::Producer ? "producer-overlap" :
                       event.kind == Kind::Recording ? "recording-overlap" : "renderpass-pairing";
    const auto& a = event.first;
    const auto& b = event.second;
    // 先输出未闭合区间；若进程随后崩溃，不能丢掉已捕获的冲突证据。
    std::fprintf(stderr,
        "GPU diagnostic: kind=%s report=%llu phase=%s reason=%s buffer=%p generation=%llu "
        "sequence=%llu A={tid=%llu name=%s entry=%s start=%llu end=%llu} "
        "B={tid=%llu name=%s entry=%s start=%llu end=%llu} (end=0 means open)\n",
        kind, static_cast<unsigned long long>(event.report), event.completed ? "closed" : "open",
        event.reason, reinterpret_cast<void*>(a.key.buffer),
        static_cast<unsigned long long>(a.key.generation),
        static_cast<unsigned long long>(event.sequence),
        static_cast<unsigned long long>(a.thread), a.name.c_str(), a.entry.c_str(),
        static_cast<unsigned long long>(a.start_ns), static_cast<unsigned long long>(a.end_ns),
        static_cast<unsigned long long>(b.thread), b.name.c_str(), b.entry.c_str(),
        static_cast<unsigned long long>(b.start_ns), static_cast<unsigned long long>(b.end_ns));
#ifndef _WIN32
    if (!event.completed) {
        void* frames[24];
        const int count = backtrace(frames, 24);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
    }
#endif
}

// 此类只保护诊断元数据；锁在返回调用方前释放，不串行化被观测的 Vulkan 操作。
// 不依赖 Vulkan，宿主测试与生产路径直接使用同一实现。
class Tracker {
public:
    using Sink = std::function<void(const Event&)>;
    explicit Tracker(Options options_, Sink sink_ = Print)
        : options{options_}, sink{std::move(sink_)} {}

    bool Enabled(Kind kind) const {
        return kind == Kind::Producer ? options.producer :
               kind == Kind::Recording ? options.recording : options.pairing;
    }

    Key NewSequence(uintptr_t buffer) {
        if (!options.recording && !options.pairing) return {};
        std::scoped_lock lock{mutex};
        const Key key{buffer, ++generation};
        sequences[buffer] = {key, 0, false};
        return key;
    }

    Key Sequence(uintptr_t buffer) {
        if (!options.recording && !options.pairing) return {};
        std::scoped_lock lock{mutex};
        const auto it = sequences.find(buffer);
        return it == sequences.end() ? Key{buffer, 0} : it->second.key;
    }

    uint64_t Enter(Kind kind, Key key, const char* entry) {
        if (!Enabled(kind)) return 0;
        Interval next{0, ThreadId(), Now(), 0, ThreadName(), entry, key, kind};
        std::scoped_lock lock{mutex};
        for (auto& interval : active) {
            if (interval.kind == kind && interval.key == key && interval.thread == next.thread) {
                ++interval.depth;
                return interval.token;
            }
        }
        next.token = ++token;
        for (const auto& other : active) {
            if (other.kind != kind || other.key != key) continue;
            auto& count = reports[static_cast<size_t>(kind)];
            if (count >= options.limit) break;
            Event event{kind, ++count, 0, "overlap", false, other, next};
            pending.push_back(event);
            sink(event);
        }
        active.push_back(std::move(next));
        return token;
    }

    void Leave(uint64_t id) {
        if (!id) return;
        std::scoped_lock lock{mutex};
        auto it = std::find_if(active.begin(), active.end(), [id](const auto& v) { return v.token == id; });
        if (it == active.end() || --it->depth) return;
        const uint64_t end = Now();
        for (auto& event : pending) {
            if (event.first.token == id) event.first.end_ns = end;
            if (event.second.token == id) event.second.end_ns = end;
            if (event.first.end_ns && event.second.end_ns) {
                event.completed = true;
                sink(event);
            }
        }
        std::erase_if(pending, [](const auto& e) { return e.completed; });
        active.erase(it);
    }

    void Rendering(uintptr_t buffer, bool begin, const char* entry) {
        if (!options.pairing) return;
        std::scoped_lock lock{mutex};
        auto it = sequences.find(buffer);
        if (it == sequences.end()) return;
        auto& state = it->second;
        ++state.serial;
        if (state.open == begin) PairError(state, begin ? "begin-while-open" : "end-without-begin", entry);
        state.open = begin;
    }

    void EndSequence(uintptr_t buffer) {
        if (!options.pairing) return;
        std::scoped_lock lock{mutex};
        const auto it = sequences.find(buffer);
        if (it != sequences.end() && it->second.open)
            PairError(it->second, "submit-with-open-renderpass", "SubmitExecution");
    }

private:
    struct SequenceState { Key key; uint64_t serial; bool open; };
    void PairError(const SequenceState& state, const char* reason, const char* entry) {
        auto& count = reports[static_cast<size_t>(Kind::Pairing)];
        if (count >= options.limit) return;
        Interval where{0, ThreadId(), Now(), 0, ThreadName(), entry, state.key, Kind::Pairing};
        sink(Event{Kind::Pairing, ++count, state.serial, reason, false, where, {}});
    }
    const Options options;
    Sink sink;
    std::mutex mutex;
    uint64_t generation{}, token{};
    std::array<unsigned, 3> reports{};
    std::vector<Interval> active;
    std::vector<Event> pending;
    std::unordered_map<uintptr_t, SequenceState> sequences;
};

class Scope {
public:
    Scope(Tracker& tracker_, Kind kind, Key key, const char* entry)
        : tracker{tracker_}, token{tracker.Enter(kind, key, entry)} {}
    ~Scope() { tracker.Leave(token); }
    Scope(Scope&& other) noexcept : tracker{other.tracker}, token{std::exchange(other.token, 0)} {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Tracker& tracker;
    uint64_t token;
};

inline Tracker& Get() {
    static Tracker tracker{[] {
        const auto enabled = [](const char* name) {
            const char* value = std::getenv(name);
            return value && value[0] == '1';
        };
        const Options options{enabled("GOW3_PRODUCER_CHECK"), enabled("GOW3_RECORDING_CHECK"),
                              enabled("GOW3_RENDERPASS_CHECK")};
        if (options.producer || options.recording || options.pairing) {
            std::fprintf(stderr,
                "GPU diagnostic: enabled producer=%d recording=%d pairing=%d limit=%u\n",
                options.producer, options.recording, options.pairing, options.limit);
        }
        return options;
    }()};
    return tracker;
}

} // namespace Vulkan::RecordingDiagnostics
