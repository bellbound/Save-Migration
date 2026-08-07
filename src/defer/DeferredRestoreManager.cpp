#include "defer/DeferredRestoreManager.h"

#include <algorithm>
#include <format>

#include "config/MigrationConfig.h"
#include "core/CategoryRegistry.h"
#include "core/Worker.h"
#include "model/FormRef.h"
#include "report/ReportSink.h"
#include "report/ReportWriter.h"
#include "util/ActorEnum.h"
#include "util/GameThread.h"
#include "util/StringUtil.h"

namespace SaveMigration::Defer {

// ═══════════════════════════════════════════════════════════════════════════
// Sinks. Each one only enqueues.
// ═══════════════════════════════════════════════════════════════════════════

/// The "NPC loaded" trigger. `EquipObject` silently no-ops on an actor without
/// 3D, so this is the gate for every equipment-shaped deferred item.
class ObjectLoadedSink final : public RE::BSTEventSink<RE::TESObjectLoadedEvent> {
public:
    static ObjectLoadedSink* Get() {
        static ObjectLoadedSink instance;
        return &instance;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* event,
                                          RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override {
        if (!event || !event->loaded) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto& queue = PendingWorkQueue::Get();
        if (queue.Empty()) {
            return RE::BSEventNotifyControl::kContinue;  // inert
        }
        auto* form = RE::TESForm::LookupByID(event->formID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const auto key = Model::FormKeyUtil::BuildFormKey(actor);
        if (!key.empty()) {
            DeferredRestoreManager::Get().NotifySubject(key, Trigger::kActorLoaded);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

/// "The player entered the NPC's home cell."
class CellAttachSink final : public RE::BSTEventSink<RE::TESCellAttachDetachEvent> {
public:
    static CellAttachSink* Get() {
        static CellAttachSink instance;
        return &instance;
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESCellAttachDetachEvent* event,
        RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override {
        if (!event || !event->attached || !event->reference) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto& queue = PendingWorkQueue::Get();
        if (queue.Empty()) {
            return RE::BSEventNotifyControl::kContinue;
        }
        if (auto* cell = event->reference->GetParentCell()) {
            const auto key = Model::FormKeyUtil::BuildFormKey(cell);
            if (!key.empty()) {
                DeferredRestoreManager::Get().NotifyCell(key, Trigger::kCellAttached);
            }
        }
        // The reference itself may be a watched actor.
        if (auto* actor = event->reference->As<RE::Actor>()) {
            const auto key = Model::FormKeyUtil::BuildFormKey(actor);
            if (!key.empty()) {
                DeferredRestoreManager::Get().NotifySubject(key, Trigger::kCellAttached);
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

/// The second, later chance. A cell can attach *before* its references have 3D,
/// which is exactly when an equip fails quietly - so this is not redundant with
/// the attach event.
class CellFullyLoadedSink final : public RE::BSTEventSink<RE::TESCellFullyLoadedEvent> {
public:
    static CellFullyLoadedSink* Get() {
        static CellFullyLoadedSink instance;
        return &instance;
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESCellFullyLoadedEvent* event,
        RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override {
        if (!event || !event->cell) {
            return RE::BSEventNotifyControl::kContinue;
        }
        if (PendingWorkQueue::Get().Empty()) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const auto key = Model::FormKeyUtil::BuildFormKey(event->cell);
        if (!key.empty()) {
            DeferredRestoreManager::Get().NotifyCell(key, Trigger::kCellFullyLoaded);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

class LoadGameSink final : public RE::BSTEventSink<RE::TESLoadGameEvent> {
public:
    static LoadGameSink* Get() {
        static LoadGameSink instance;
        return &instance;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent*,
                                          RE::BSTEventSource<RE::TESLoadGameEvent>*) override {
        DeferredRestoreManager::Get().OnGameLoaded();
        return RE::BSEventNotifyControl::kContinue;
    }
};

// ═══════════════════════════════════════════════════════════════════════════

DeferredRestoreManager& DeferredRestoreManager::Get() {
    static DeferredRestoreManager instance;
    return instance;
}

void DeferredRestoreManager::RegisterSinks() {
    if (m_sinksRegistered) {
        return;
    }
    auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!holder) {
        spdlog::error("DeferredRestoreManager: no ScriptEventSourceHolder; deferred work disabled");
        return;
    }
    holder->AddEventSink<RE::TESObjectLoadedEvent>(ObjectLoadedSink::Get());
    holder->AddEventSink<RE::TESCellAttachDetachEvent>(CellAttachSink::Get());
    holder->AddEventSink<RE::TESCellFullyLoadedEvent>(CellFullyLoadedSink::Get());
    holder->AddEventSink<RE::TESLoadGameEvent>(LoadGameSink::Get());
    m_sinksRegistered = true;
    spdlog::info("DeferredRestoreManager: 4 sinks registered (inert while the queue is empty)");
}

void DeferredRestoreManager::NotifySubject(const std::string& subjectKey, Trigger trigger) {
    // One hash probe against a set rebuilt only on queue mutation.
    if (!PendingWorkQueue::Get().WatchedSubjects().contains(subjectKey)) {
        return;
    }
    {
        std::lock_guard lock(m_readyMutex);
        m_ready.emplace_back(subjectKey, TriggerBits(trigger));
    }
    ScheduleDrain();
}

void DeferredRestoreManager::NotifyCell(const std::string& cellKey, Trigger trigger) {
    if (!PendingWorkQueue::Get().WatchedCells().contains(cellKey)) {
        return;
    }
    {
        std::lock_guard lock(m_readyMutex);
        m_ready.emplace_back(cellKey, TriggerBits(trigger));
    }
    ScheduleDrain();
}

void DeferredRestoreManager::ScheduleDrain() {
    bool expected = false;
    if (!m_drainScheduled.compare_exchange_strong(expected, true)) {
        return;  // a drain is already queued: coalesce into it
    }
    Util::OnGameThread([this]() {
        m_drainScheduled.store(false);
        Drain();
    });
}

void DeferredRestoreManager::OnGameLoaded() {
    if (PendingWorkQueue::Get().Empty()) {
        return;
    }
    {
        std::lock_guard lock(m_readyMutex);
        // A load makes every item worth re-testing, not just the watched ones.
        m_ready.emplace_back(std::string{}, TriggerBits(Trigger::kGameLoaded));
    }
    ScheduleDrain();
}

void DeferredRestoreManager::ForceDrain() {
    {
        std::lock_guard lock(m_readyMutex);
        m_ready.emplace_back(std::string{}, 0xFF);  // all triggers
    }
    ScheduleDrain();
}

bool DeferredRestoreManager::ApplyItem(PendingItem& item, Report::ReportSink& sink) {
    auto* category = Core::CategoryRegistry::Get().FindActor(item.categoryId);
    if (!category) {
        spdlog::warn("DeferredRestoreManager: no category '{}' in this build; retiring item",
                     item.categoryId);
        sink.Failed(Report::SystemSubject(item.categoryId), item.subjectFormKey,
                    Report::ReasonCode::kModNotInstalled,
                    std::format("category '{}' is not present in this build", item.categoryId));
        return true;
    }

    Report::ReasonCode reason = Report::ReasonCode::kNone;
    auto* actor = Model::FormResolver::Get().ResolveChecked<RE::Actor>(item.subjectFormKey, reason);
    if (!actor) {
        sink.Failed(Report::SystemSubject(item.subjectFormKey), item.subjectFormKey,
                    reason == Report::ReasonCode::kNone ? Report::ReasonCode::kSubjectUnresolvable
                                                        : reason,
                    "the deferred subject could not be resolved in this session");
        return true;  // never going to resolve: retire
    }

    Model::ActorSubject subject;
    subject.actor = actor;
    subject.base = actor->GetActorBase();
    subject.refKey = item.subjectFormKey;
    subject.baseKey = Model::FormKeyUtil::BuildFormKey(subject.base);
    const char* name = actor->GetName();
    subject.displayName = (name && *name) ? Util::ConvertSkyrimTextToUTF8(name) : item.subjectFormKey;

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};

    // Readiness gate. Anything wanting kActorLoaded genuinely needs 3D.
    if (item.WantsTrigger(Trigger::kActorLoaded) && !Util::ActorEnum::IsReadyForEquip(actor)) {
        ++item.attempts;
        return false;  // retry on the next trigger
    }

    // The payload is self-contained, so a synthetic document is enough: the
    // queue never depends on the snapshot directory still existing.
    Model::SnapshotDocument synthetic;
    auto parsed = nlohmann::json::parse(item.payload, nullptr, false);
    if (parsed.is_discarded()) {
        sink.Failed(subjectRef, item.subjectFormKey, Report::ReasonCode::kIoError,
                    "the deferred payload was not valid JSON");
        return true;
    }
    synthetic.actorCategories[item.categoryId] = {
        {"byActor", {{item.subjectFormKey, std::move(parsed)}}}};

    static const std::vector<std::string> kNoMissingPlugins;
    std::vector<Model::ActorSubject> subjects{subject};
    bool continuation = false;
    Core::ApplyContext ctx{synthetic,
                           sink,
                           PendingWorkQueue::Get(),
                           kNoMissingPlugins,
                           &subjects,
                           RE::PlayerCharacter::GetSingleton(),
                           &continuation};

    const auto& descriptor = category->Describe();
    sink.BeginCategory(descriptor.id, descriptor.displayName, Core::PhaseValue(descriptor.phase));
    bool retire = true;
    try {
        retire = category->ApplyDeferred(subject, ctx);
    } catch (const std::exception& e) {
        sink.Failed(subjectRef, item.subjectFormKey, Report::ReasonCode::kIoError,
                    std::format("deferred applier threw: {}", e.what()));
        retire = true;
    }
    sink.EndCategory();

    if (!retire) {
        ++item.attempts;
    }
    return retire;
}

void DeferredRestoreManager::Drain() {
    std::vector<std::pair<std::string, uint8_t>> ready;
    {
        std::lock_guard lock(m_readyMutex);
        ready.swap(m_ready);
    }
    if (ready.empty()) {
        return;
    }

    auto& queue = PendingWorkQueue::Get();
    auto items = queue.Items();
    if (items.empty()) {
        return;
    }

    // An empty key from the load/force path means "consider everything".
    uint8_t globalTriggers = 0;
    std::unordered_map<std::string, uint8_t> perKey;
    for (const auto& [key, bits] : ready) {
        if (key.empty()) {
            globalTriggers |= bits;
        } else {
            perKey[key] |= bits;
        }
    }

    auto* calendar = RE::Calendar::GetSingleton();
    const float gameDays = calendar ? calendar->GetDaysPassed() : 0.0f;

    std::shared_ptr<Report::ReportSink> sink;
    {
        std::lock_guard lock(m_supplementMutex);
        if (!m_supplementSink) {
            m_supplementSink = std::make_shared<Report::ReportSink>();
            m_supplementSink->SetHeader("import", "", "", "", "", gameDays, 0);
        }
        sink = m_supplementSink;
    }

    std::vector<PendingItem> survivors;
    survivors.reserve(items.size());
    uint32_t retired = 0;

    for (auto& item : items) {
        uint8_t triggers = globalTriggers;
        if (const auto it = perKey.find(item.subjectFormKey); it != perKey.end()) {
            triggers |= it->second;
        }
        if (const auto it = perKey.find(item.cellFormKey); it != perKey.end()) {
            triggers |= it->second;
        }
        if ((triggers & item.trigger) == 0) {
            survivors.push_back(std::move(item));
            continue;
        }

        // Expiry before work: a stale item should not touch the world.
        if (item.expiresAtGameTime > 0.0f && gameDays > item.expiresAtGameTime) {
            sink->BeginCategory(item.categoryId, item.categoryId, 0);
            sink->Failed(Report::SystemSubject(item.subjectFormKey), item.subjectFormKey,
                         Report::ReasonCode::kDeferredExpired,
                         std::format("passed its game-day deadline of {:.2f}",
                                     item.expiresAtGameTime));
            sink->EndCategory();
            ++retired;
            m_anythingRetired = true;
            continue;
        }

        if (ApplyItem(item, *sink)) {
            ++retired;
            m_anythingRetired = true;
            continue;
        }

        if (item.attempts >= item.maxAttempts) {
            sink->BeginCategory(item.categoryId, item.categoryId, 0);
            sink->Failed(Report::SystemSubject(item.subjectFormKey), item.subjectFormKey,
                         Report::ReasonCode::kDeferredExhausted,
                         std::format("{} attempt(s) without the subject becoming ready",
                                     item.attempts));
            sink->EndCategory();
            ++retired;
            m_anythingRetired = true;
            continue;
        }
        survivors.push_back(std::move(item));
    }

    queue.Replace(std::move(survivors));
    if (retired > 0) {
        spdlog::info("DeferredRestoreManager: retired {} item(s), {} remaining", retired,
                     queue.Size());
    }

    if (queue.Empty() && m_anythingRetired) {
        // Queue empty -> sinks go inert (they short-circuit on Empty()) and the
        // supplement report is written.
        WriteSupplement();
    }
}

void DeferredRestoreManager::WriteSupplement() {
    std::shared_ptr<Report::ReportSink> sink;
    {
        std::lock_guard lock(m_supplementMutex);
        if (!m_supplementSink) {
            return;
        }
        sink = std::move(m_supplementSink);
        m_anythingRetired = false;
    }

    auto report = sink->Finish();
    Core::Worker::Get().Post("deferred-supplement", [report]() {
        const auto rendered = Report::ReportWriter::Render(report);
        Report::ReportWriter::Write(report, rendered, "deferred");
    });
}

}  // namespace SaveMigration::Defer
