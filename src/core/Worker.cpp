#include "core/Worker.h"

#include <chrono>

namespace SaveMigration::Core {

Worker& Worker::Get() {
    static Worker instance;
    return instance;
}

Worker::~Worker() { Shutdown(); }

void Worker::EnsureStarted() {
    // Caller holds m_mutex.
    if (m_running || m_stopping) {
        return;
    }
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    spdlog::debug("Worker: thread started");
}

void Worker::Post(std::string_view label, std::function<void()> task) {
    if (!task) {
        return;
    }
    std::unique_lock lock(m_mutex);
    if (m_stopping) {
        spdlog::warn("Worker: dropping task '{}' during shutdown", label);
        return;
    }
    EnsureStarted();
    m_queue.push_back(Task{std::string(label), std::move(task)});
    lock.unlock();
    m_cv.notify_one();
}

void Worker::Run() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) {
                // Only exit on an empty queue, so Shutdown() drains rather than
                // abandoning half-written output.
                if (m_stopping) {
                    return;
                }
                continue;
            }
            task = std::move(m_queue.front());
            m_queue.pop_front();
            m_taskInFlight = true;
        }

        const auto started = std::chrono::steady_clock::now();
        try {
            task.fn();
        } catch (const std::exception& e) {
            spdlog::error("Worker: task '{}' threw: {}", task.label, e.what());
        } catch (...) {
            spdlog::error("Worker: task '{}' threw a non-std exception", task.label);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        spdlog::debug("Worker: task '{}' finished in {} ms", task.label, elapsed);

        {
            std::lock_guard lock(m_mutex);
            m_taskInFlight = false;
        }
    }
}

void Worker::Shutdown() {
    std::thread toJoin;
    {
        std::lock_guard lock(m_mutex);
        if (!m_running) {
            m_stopping = true;
            return;
        }
        m_stopping = true;
        toJoin = std::move(m_thread);
        m_running = false;
    }
    m_cv.notify_all();
    if (toJoin.joinable()) {
        toJoin.join();
        spdlog::debug("Worker: thread joined");
    }
}

bool Worker::IsBusy() const {
    std::lock_guard lock(m_mutex);
    return m_taskInFlight || !m_queue.empty();
}

size_t Worker::QueueDepth() const {
    std::lock_guard lock(m_mutex);
    return m_queue.size();
}

}  // namespace SaveMigration::Core
