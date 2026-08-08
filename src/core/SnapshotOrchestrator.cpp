#include "core/SnapshotOrchestrator.h"

#include <chrono>
#include <format>

#include "config/MigrationConfig.h"
#include "core/CategoryRegistry.h"
#include "core/MigrationState.h"
#include "core/SaveIdentity.h"
#include "core/VRLayoutProbe.h"
#include "core/Worker.h"
#include "model/SnapshotDocument.h"
#include "papyrus/ModProbe.h"
#include "papyrus/PapyrusInterface.h"
#include "report/ReportSink.h"
#include "report/ReportWriter.h"
#include "store/LoadOrderFingerprint.h"
#include "store/SnapshotPaths.h"
#include "store/SnapshotWriter.h"
#include "util/ActorEnum.h"
#include "util/GameThread.h"
#include "util/StringUtil.h"

namespace SaveMigration::Core {

namespace {

int64_t NowUnixMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

float CurrentGameDays() {
    auto* calendar = RE::Calendar::GetSingleton();
    return calendar ? calendar->GetDaysPassed() : 0.0f;
}

}  // namespace

SnapshotOrchestrator& SnapshotOrchestrator::Get() {
    static SnapshotOrchestrator instance;
    return instance;
}

std::string SnapshotOrchestrator::StateKey() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    const auto level = player ? player->GetLevel() : 0;
    // 1e-4 days is about 8.6 seconds of game time - fine enough that real play
    // always moves it, coarse enough that float noise never does.
    return std::format("{}|{:.4f}|{}", SaveIdentity::Get().SaveId(), CurrentGameDays(), level);
}

bool SnapshotOrchestrator::ShouldTake(std::string& reasonOut) {
    if (!Config::MigrationConfig::IsSnapshotMode()) {
        reasonOut = "bSnapshot=0";
        return false;
    }
    if (SaveIdentity::Get().SaveId().empty()) {
        reasonOut = "no save id yet";
        return false;
    }
    if (m_inFlight.load()) {
        reasonOut = "a snapshot is already in flight";
        return false;
    }
    if (MigrationState::Get().HasFlag(StateFlag::kRestoreInProgress)) {
        // A half-restored world is not a state worth preserving.
        reasonOut = "a restore is in progress";
        return false;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        reasonOut = "no player";
        return false;
    }
    // "Chargen complete" is asserted with offset-immune signals only. The
    // obvious candidates - byCharGenFlag, numberOfDaysUsedInPeriod - both live in
    // PLAYER_RUNTIME_DATA, the block whose VR offsets the layout probe exists to
    // distrust. 3D-loaded plus a non-zero clock says the same thing safely.
    if (!player->Is3DLoaded()) {
        reasonOut = "player has no 3D yet";
        return false;
    }
    const float gameDays = CurrentGameDays();
    if (!(gameDays > 0.0f)) {
        reasonOut = "game time is zero (chargen not complete)";
        return false;
    }

    const auto now = NowUnixMs();
    const auto intervalMs = static_cast<int64_t>(Config::MigrationConfig::MinSnapshotIntervalSec()) * 1000;
    if (m_lastSnapshotUnixMs != 0 && (now - m_lastSnapshotUnixMs) < intervalMs) {
        reasonOut = std::format("only {} ms since the last snapshot (minimum {} ms)",
                                now - m_lastSnapshotUnixMs, intervalMs);
        return false;
    }

    // The anti-thrash gate proper.
    const auto key = StateKey();
    if (key == m_lastStateKey) {
        reasonOut = std::format("state unchanged since the last snapshot ({})", key);
        return false;
    }

    reasonOut.clear();
    return true;
}

void SnapshotOrchestrator::Take(std::string_view savePath) {
    bool expected = false;
    if (!m_inFlight.compare_exchange_strong(expected, true)) {
        spdlog::warn("SnapshotOrchestrator: Take() while already in flight");
        return;
    }
    Util::OnGameThread([this, path = std::string(savePath)]() mutable {
        m_harvestStarted = false;
        m_probeOutstanding.reset();
        m_vmWaitNote.clear();
        AwaitVm(std::move(path), 0);
    });
}

void SnapshotOrchestrator::AwaitVm(std::string savePath, uint32_t attempt) {
    if (m_harvestStarted) {
        return;
    }

    const auto timeoutSec = Config::MigrationConfig::VmReadyTimeoutSec();
    if (timeoutSec <= 0) {
        OnVmReady(std::move(savePath), "wait disabled (iVmReadyTimeoutSec=0)");
        return;
    }

    const auto maxAttempts =
        static_cast<uint32_t>((static_cast<int64_t>(timeoutSec) * 1000) / kVmProbeIntervalMs);
    if (attempt >= maxAttempts) {
        // Harvesting a document with some VM-sourced fields missing beats
        // harvesting nothing, so this is a warning and not an abort - but it has
        // to be visible, because it is exactly the condition that silently
        // produced a "capture pending" TNG payload.
        spdlog::warn(
            "SnapshotOrchestrator: Papyrus VM never answered within {} s; harvesting anyway. "
            "Categories that read another mod through Papyrus may record nothing.",
            timeoutSec);
        OnVmReady(std::move(savePath),
                  std::format("timed out after {} s - VM never answered", timeoutSec));
        return;
    }

    // Cheap engine-side checks first. While a loading screen is up or the game is
    // paused by a modal there is no point dispatching at all: the call would just
    // sit in the VM's queue.
    auto* ui = RE::UI::GetSingleton();
    const bool loading = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
    const bool paused = ui && ui->GameIsPaused();

    if (!loading && !paused) {
        if (!m_probeOutstanding || !m_probeOutstanding->load()) {
            auto outstanding = std::make_shared<std::atomic<bool>>(true);
            m_probeOutstanding = outstanding;

            // `Utility.IsInMenuMode` is the probe because it is a vanilla global
            // native that answers cheaply, and its answer is *also* the menu
            // check - a VM that replies "not in menu mode" has both resumed and
            // left the blocking prompt behind.
            const bool dispatched =
                Papyrus::PapyrusInterface::GetSingleton()->CallGlobalFunctionBool(
                    "Utility", "IsInMenuMode", {},
                    [this, savePath, outstanding](bool inMenuMode) {
                        outstanding->store(false);
                        if (inMenuMode) {
                            return;  // VM is alive but a menu still owns the screen
                        }
                        Util::OnGameThread([this, savePath]() mutable {
                            OnVmReady(std::move(savePath), "Utility.IsInMenuMode answered false");
                        });
                    });
            if (!dispatched) {
                outstanding->store(false);
            }
        }
    }

    Util::OnGameThreadAfter(kVmProbeIntervalMs,
                            [this, savePath = std::move(savePath), attempt]() mutable {
                                AwaitVm(std::move(savePath), attempt + 1);
                            });
}

void SnapshotOrchestrator::OnVmReady(std::string savePath, std::string_view detail) {
    if (m_harvestStarted) {
        return;  // a queued probe answering late
    }
    m_harvestStarted = true;

    const auto settleMs = static_cast<uint32_t>(Config::MigrationConfig::VmSettleDelayMs());
    m_vmWaitNote = std::format("{}; settled {} ms", detail, settleMs);
    spdlog::info("SnapshotOrchestrator: VM ready ({}), harvesting in {} ms", detail, settleMs);

    // Let every category dispatch its VM round-trips *now*, so the settle delay
    // is the window their answers land in. A category that dispatches from
    // Collect instead gets its answer after the document is already serialised.
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto& registry = CategoryRegistry::Get();
    uint32_t primed = 0;
    for (const auto& entry : registry.Ordered()) {
        const auto& descriptor = entry.Describe();
        if (registry.IsDisabled(descriptor.id) || !entry.IsAvailable()) {
            continue;
        }
        try {
            if (entry.global) {
                entry.global->PrepareCollect(player);
            } else {
                entry.actor->PrepareCollect(player);
            }
            ++primed;
        } catch (const std::exception& e) {
            spdlog::error("SnapshotOrchestrator: PrepareCollect for '{}' threw: {}", descriptor.id,
                          e.what());
        }
    }
    spdlog::debug("SnapshotOrchestrator: primed {} available category/categories", primed);

    if (settleMs == 0) {
        // One AddTask for the whole harvest: internal consistency depends on the
        // world not advancing partway through.
        RunHarvest(std::move(savePath));
        return;
    }
    Util::OnGameThreadAfter(settleMs, [this, savePath = std::move(savePath)]() mutable {
        RunHarvest(std::move(savePath));
    });
}

void SnapshotOrchestrator::ContributeRosterSource(Util::ActorEnum::ExtraSource source) {
    if (source.refKeys.empty()) {
        return;
    }
    m_pendingExtraSources.push_back(std::move(source));
}

void SnapshotOrchestrator::ForceTake(std::string_view savePath) {
    spdlog::warn("SnapshotOrchestrator: forced snapshot (gates bypassed)");
    m_lastStateKey.clear();
    m_lastSnapshotUnixMs = 0;
    Take(savePath);
}

void SnapshotOrchestrator::RunHarvest(std::string savePath) {
    const auto started = std::chrono::steady_clock::now();

    auto doc = std::make_shared<Model::SnapshotDocument>();
    auto sink = std::make_shared<Report::ReportSink>();

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        spdlog::error("SnapshotOrchestrator: no player at harvest time");
        m_inFlight.store(false);
        return;
    }

    // ── Manifest fields ───────────────────────────────────────────────────
    doc->manifestSchemaVersion = Model::kManifestSchemaVersion;
    doc->takenAtUnixMs = NowUnixMs();
    doc->saveId = SaveIdentity::Get().SaveId();
    doc->savePath = savePath;
    doc->gameTimeDays = CurrentGameDays();
    doc->playerLevel = player->GetLevel();
    doc->gameRuntime = REL::Module::IsVR() ? "VR" : (REL::Module::IsAE() ? "AE" : "SE");
    doc->layoutSuspect = VRLayoutProbe::Get().IsLayoutTrusted() ? 0u : 1u;
    doc->diagnostics["vrLayoutProbe"] = VRLayoutProbe::Get().Detail();
    // How the VM wait ended. A snapshot taken after a timeout is still a
    // snapshot, but anything sourced through Papyrus in it is suspect, and the
    // importer should be able to see that without reading the log.
    doc->diagnostics["vmWait"] = m_vmWaitNote;

    if (auto* base = player->GetActorBase()) {
        const char* name = base->GetFullName();
        doc->characterName = (name && *name) ? Util::ConvertSkyrimTextToUTF8(name) : "Unnamed";
    } else {
        doc->characterName = "Unnamed";
    }

    auto& fingerprint = Store::LoadOrderFingerprint::Get();
    if (!fingerprint.IsCaptured()) {
        fingerprint.CaptureCurrent();
    }
    doc->loadOrder = fingerprint.ToJson();

    sink->SetHeader("export", doc->saveId, "", doc->savePath, doc->characterName, doc->gameTimeDays,
                    doc->playerLevel);

    // ── Roster ────────────────────────────────────────────────────────────
    // Integration roster sources are contributed by the integration categories
    // through their own collectors; ActorEnum takes whatever they hand over.
    const auto subjects = Util::ActorEnum::BuildForCollect(m_pendingExtraSources);
    m_pendingExtraSources.clear();
    doc->roster = Util::ActorEnum::RosterToJson(subjects);

    const auto playerSubject = Util::ActorEnum::PlayerSubject();

    CollectContext ctx{*doc, *sink, player, &subjects};

    // ── Every category, in the one declared order ──────────────────────────
    // Per-actor categories walk the roster once each; the roster itself was built
    // once above, rather than each of ~13 categories re-discovering it.
    auto& registry = CategoryRegistry::Get();
    for (const auto& entry : registry.Ordered()) {
        const auto& descriptor = entry.Describe();
        sink->BeginCategory(descriptor.id, descriptor.displayName, PhaseValue(descriptor.phase));

        if (registry.IsDisabled(descriptor.id)) {
            sink->SkipCategory(Report::ReasonCode::kSkippedByIni, "disabled in the INI");
        } else if (!entry.IsAvailable()) {
            sink->SkipCategory(
                Report::ReasonCode::kModNotInstalled,
                std::format("unavailable: missing {}",
                            Papyrus::ModProbe::Get().FirstMissing(descriptor.requirement)));
        } else {
            try {
                if (entry.global) {
                    entry.global->Collect(ctx);
                } else {
                    entry.actor->BeginCollect(ctx);
                    entry.actor->CollectActor(playerSubject, ctx);
                    for (const auto& subject : subjects) {
                        entry.actor->CollectActor(subject, ctx);
                    }
                    entry.actor->EndCollect(ctx);
                }
            } catch (const std::exception& e) {
                sink->FailCategory(Report::ReasonCode::kIoError,
                                   std::format("collector threw: {}", e.what()));
            }
        }
        sink->EndCategory();
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    spdlog::info("SnapshotOrchestrator: harvest of {} category/categories took {} ms",
                 registry.TotalCount(), elapsedMs);
    doc->diagnostics["harvestMs"] = elapsedMs;

    // Record the gate values only once the harvest actually happened.
    m_lastStateKey = StateKey();
    m_lastSnapshotUnixMs = NowUnixMs();

    const auto snapshotDir = Store::SnapshotPaths::SnapshotDir(doc->saveId, doc->characterName);

    // ── B1 boundary: everything past here is file work on the worker ───────
    Worker::Get().Post("snapshot-write", [this, doc, sink, snapshotDir]() {
        auto report = sink->Finish();
        const auto writeResult = Store::SnapshotWriter::Write(snapshotDir, *doc);
        if (!writeResult.success) {
            spdlog::error("SnapshotOrchestrator: write failed: {}", writeResult.error);
        }

        const auto rendered = Report::ReportWriter::Render(report);
        Report::ReportWriter::Write(report, rendered);
        // Also inside the snapshot, so a snapshot directory explains itself.
        Store::SnapshotWriter::WriteReportCopy(snapshotDir, rendered.text, rendered.json);

        m_inFlight.store(false);
    });
}

}  // namespace SaveMigration::Core
