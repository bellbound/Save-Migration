#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace SaveMigration::Core {

/// One owned background thread with a FIFO task queue.
///
/// Owned and joined, not detached: a detached thread writing a snapshot
/// directory while the process tears down is how you get a truncated manifest
/// that still parses. `Shutdown()` drains what has been queued and then joins.
///
/// Everything file-shaped runs here - JSON build and `SafeDump`, all file I/O,
/// directory scans, side-car copies, both report writers, and all SQLite work.
/// Nothing here may touch the engine; the game thread is reached only by posting
/// back through `Util::OnGameThread`.
class Worker {
public:
    static Worker& Get();

    /// Starts the thread on first use.
    void Post(std::string_view label, std::function<void()> task);

    /// Drain and join. Safe to call more than once.
    void Shutdown();

    [[nodiscard]] bool IsBusy() const;
    [[nodiscard]] size_t QueueDepth() const;

    ~Worker();

private:
    Worker() = default;
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void EnsureStarted();
    void Run();

    struct Task {
        std::string label;
        std::function<void()> fn;
    };

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Task> m_queue;
    std::thread m_thread;
    bool m_running = false;
    bool m_stopping = false;
    bool m_taskInFlight = false;
};

}  // namespace SaveMigration::Core
