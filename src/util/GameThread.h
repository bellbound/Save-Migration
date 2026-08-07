#pragma once

#include <functional>

namespace SaveMigration::Util {

/// Run `fn` on the game thread via SKSE's task interface.
///
/// Every engine mutation in this plugin goes through here. The interface is
/// fetched per call rather than cached because it is null until SKSE::Init has
/// run, and several call sites are reachable from static initialisation order
/// we do not control.
inline void OnGameThread(std::function<void()> fn) {
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) {
        spdlog::error("GameThread: task interface unavailable, dropping task");
        return;
    }
    tasks->AddTask([fn = std::move(fn)]() {
        try {
            fn();
        } catch (const std::exception& e) {
            // An exception escaping into the engine's frame loop is a crash.
            spdlog::error("GameThread: task threw: {}", e.what());
        } catch (...) {
            spdlog::error("GameThread: task threw a non-std exception");
        }
    });
}

}  // namespace SaveMigration::Util
