#include "util/Notice.h"

#include "core/RestoreOrchestrator.h"
#include "util/GameThread.h"

namespace SaveMigration::Util::Notice {

namespace {

void Emit(std::string_view source, std::string text) {
    // Checked on the game thread rather than at the call site. Both callers are
    // worker jobs whose completion time is not bounded by anything - a large
    // SkyrimNet database copy can outlive the import pass by seconds - so the
    // answer at dispatch time is not the answer that matters.
    Util::OnGameThread([source = std::string(source), text = std::move(text)]() {
        if (!Core::RestoreOrchestrator::Get().IsRunning()) {
            spdlog::info("{}: withheld an on-screen notice, the restore has already finished: {}",
                         source.empty() ? "Notice" : source, text);
            return;
        }
        RE::DebugNotification(text.c_str());
    });
}

}  // namespace

void DuringRestore(std::string text) { Emit({}, std::move(text)); }

void DuringRestore(std::string_view source, std::string text) { Emit(source, std::move(text)); }

}  // namespace SaveMigration::Util::Notice
