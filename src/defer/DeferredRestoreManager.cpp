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

namespace {

/// How many items may touch the world in one drain before the rest wait a frame.
///
/// Counted only in items that got *past* their readiness gate, because those are
/// the ones that cost: a deferred equipment replay walks 32 biped slots resolving
/// and equipping, and an outfit replay calls into another mod. An item that turns
/// out not to be ready is one form lookup and a bail, so it is not budgeted at
/// all - the whole queue can be tested in a frame without anyone noticing.
///
/// The cap exists because the queue holds up to 512 items and a great many of
/// them can become eligible in the same frame: walk into a settlement holding a
/// dozen queued followers, or load a save. In VR a dropped frame is not a
/// stutter, it is nausea, so the work spreads across frames instead.
///
/// Deliberately not `iItemsPerFrame`: that knob is about inventory *items* inside
/// one category's chunked apply, which is far cheaper per unit than a whole
/// deferred replay, and it defaults to 200.
constexpr uint32_t kMaxAppliesPerDrain = 8;

/// The player-facing sentence for an item still on the queue when the import ends.
///
/// One table rather than a line each category writes at enqueue time, because at
/// enqueue time nobody knows yet whether the item will survive the settle pass -
/// and `ReportSink::ClaimBucket` allows one bucket per item id for the whole run,
/// so a premature "this is waiting" could never be corrected to "this landed".
///
/// Keyed by category id, with a default that is true of all of them, so a new
/// deferring category reports something sensible without an edit here.
std::string_view RemainingNotice(std::string_view categoryId) {
    if (categoryId == "npc.equipment") {
        return "the engine took the equip and the item did not read back as worn, which is what an "
               "unrendered actor looks like, so their gear goes on when you next see them";
    }
    if (categoryId == "npc.obody_preset") {
        return "their preset key is already written; the morph itself needs the body rendered, and "
               "ORefit derives clothing morphs from worn slot 32, so it lands the first time you "
               "see them";
    }
    if (categoryId == "npc.outfit_vr_dressup") {
        return "the outfit is already in VR Dress Up's storage; the equip waits until they load so "
               "ApplyOutfit cannot prune it against an unfilled inventory";
    }
    if (categoryId == "npc.home_mhiyh") {
        return "MHIYH's free alias is kLoadedOnly, so filling it for an unloaded actor would "
               "silently do nothing - it is assigned when you next see them";
    }
    if (categoryId == "npc.skyrimnet_accompany") {
        return "SkyrimNet's follow package is registered when they load: registering a package "
               "changes AI, so it has to be the last thing done to them";
    }
    if (categoryId == "npc.fertility") {
        return "their Fertility Mode faction rank is re-asserted on the next load";
    }
    if (categoryId == "npc.tng") {
        return "the TNG addon waits for the player's 3D and for TNG's own post-load pass, so we "
               "are the last writer";
    }
    return "it is applied when you next see them";
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Sinks. Each one only enqueues.
//
// They run inside the engine's own event dispatch, once per reference, for as
// long as the queue is non-empty - which for an ongoing restore can be every
// frame of every session until the player has met the last queued NPC. Each is
// therefore ordered cheapest-test-first, and nothing allocates before a test has
// proved there is something to match it against.
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
        if (!queue.HasWatchedSubjects()) {
            return RE::BSEventNotifyControl::kContinue;  // inert
        }
        auto* form = RE::TESForm::LookupByID(event->formID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor) {
            return RE::BSEventNotifyControl::kContinue;
        }
        // Only now is a FormKey worth building: it walks `sourceFiles` calling
        // `LookupForm` per candidate and formats a string, so it is far too
        // expensive to do unconditionally on an engine event.
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
        // The busiest of the four by a wide margin: one call per *reference*, so
        // a cell transition in a heavy load order runs it thousands of times
        // across a handful of frames.
        if (queue.HasWatchedCells()) {
            if (auto* cell = event->reference->GetParentCell()) {
                // Every reference in a cell yields the same cell key, so building
                // it per reference is one answer recomputed thousands of times.
                // Memoised against the cell's FormID - not its pointer, which the
                // engine may reuse for a different cell after a load - and against
                // the queue generation. Game thread only, which is where cell
                // attach is dispatched from.
                const auto cellId = cell->GetFormID();
                const auto generation = queue.Generation();
                if (cellId != m_lastCellId || generation != m_lastCellGeneration) {
                    m_lastCellId = cellId;
                    m_lastCellGeneration = generation;
                    m_lastCellKey = Model::FormKeyUtil::BuildFormKey(cell);
                }
                if (!m_lastCellKey.empty()) {
                    DeferredRestoreManager::Get().NotifyCell(m_lastCellKey, Trigger::kCellAttached);
                }
            }
        }
        // The reference itself may be a watched actor.
        if (queue.HasWatchedSubjects()) {
            if (auto* actor = event->reference->As<RE::Actor>()) {
                const auto key = Model::FormKeyUtil::BuildFormKey(actor);
                if (!key.empty()) {
                    DeferredRestoreManager::Get().NotifySubject(key, Trigger::kCellAttached);
                }
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

private:
    RE::FormID m_lastCellId = 0;
    uint64_t m_lastCellGeneration = 0;
    std::string m_lastCellKey;
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
        if (!PendingWorkQueue::Get().HasWatchedCells()) {
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

void DeferredRestoreManager::Signal(ReadySignal signal) {
    {
        std::lock_guard lock(m_readyMutex);
        m_ready.push_back(std::move(signal));
    }
    ScheduleDrain();
}

void DeferredRestoreManager::NotifySubject(const std::string& subjectKey, Trigger trigger) {
    // One hash probe against a set rebuilt only on queue mutation, done *inside*
    // the queue so the set is never copied out. See the note on the sink hot
    // path in PendingWorkQueue.h.
    if (!PendingWorkQueue::Get().IsWatchedSubject(subjectKey)) {
        return;
    }
    // A real world event, so a subject that turns out still not to be ready has
    // genuinely had its chance and burns an attempt.
    Signal(ReadySignal{subjectKey, TriggerBits(trigger), /*matchAll=*/false,
                       /*countsAsAttempt=*/true});
}

void DeferredRestoreManager::NotifyCell(const std::string& cellKey, Trigger trigger) {
    if (!PendingWorkQueue::Get().IsWatchedCell(cellKey)) {
        return;
    }
    Signal(ReadySignal{cellKey, TriggerBits(trigger), /*matchAll=*/false,
                       /*countsAsAttempt=*/true});
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
    // A load makes every item worth re-testing whatever triggers it asked for,
    // hence `matchAll`. This used to be signalled as a `kGameLoaded` trigger
    // *bit*, which no category ever sets on an item - so the mask never matched
    // anything and the documented "kick the queue on load" did nothing at all.
    //
    // It must not burn an attempt: it fires for every queued item on every load
    // regardless of where the player is, so counting it would retire the whole
    // queue after `maxAttempts` loads without a single subject having been seen -
    // discarding precisely the work that is waiting for the player to travel.
    // `releaseImmediate`: a load is a perfectly good moment for work whose only
    // gate was "after the equipment churn", and the churn it was ordered behind
    // finished in the session that queued it.
    Signal(ReadySignal{std::string{}, 0, /*matchAll=*/true, /*countsAsAttempt=*/false,
                       /*releaseImmediate=*/true});
}

void DeferredRestoreManager::ForceDrain() {
    // The debug native. Ignores the trigger mask entirely, and for the same
    // reason as the load path it does not consume attempts: a tester poking the
    // queue should not be able to exhaust it.
    Signal(ReadySignal{std::string{}, 0xFF, /*matchAll=*/true, /*countsAsAttempt=*/false,
                       /*releaseImmediate=*/true});
}

bool DeferredRestoreManager::ApplyItem(PendingItem& item, Report::ReportSink& sink,
                                       bool countsAsAttempt, bool releaseImmediate,
                                       bool& didWork) {
    didWork = false;
    auto* category = Core::CategoryRegistry::Get().FindActor(item.categoryId);
    if (!category) {
        spdlog::warn("DeferredRestoreManager: no category '{}' in this build; retiring item",
                     item.categoryId);
        sink.Failed(Report::SystemSubject(item.categoryId), item.subjectFormKey,
                    Report::ReasonCode::kModNotInstalled,
                    std::format("category '{}' is not present in this build", item.categoryId));
        return true;
    }

    if (!Config::MigrationConfig::IsImportEnabled(item.categoryId)) {
        // Queued by an earlier session, switched off in `[Imports]` since. Retire
        // rather than retry: the switch is the user's answer, and a queue item
        // that can never run would otherwise sit there until it expired.
        spdlog::info("DeferredRestoreManager: '{}' is switched off in [Imports]; retiring item",
                     item.categoryId);
        sink.SkippedItem(Report::SystemSubject(item.categoryId), item.subjectFormKey,
                         Report::ReasonCode::kSkippedByIni,
                         std::format("switched off in [Imports] ({}=0)",
                                     Config::MigrationConfig::ImportKeyFor(item.categoryId)));
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

    // Readiness gate, and deliberately *before* the subject is built. This is the
    // branch nearly every item takes on nearly every drain, so it has to stay
    // cheap: building the subject costs another FormKey construction and a text
    // conversion, all of it wasted on an actor about to be deferred again.
    if (item.WantsTrigger(Trigger::kActorLoaded) && !Util::ActorEnum::IsReadyForEquip(actor)) {
        if (countsAsAttempt) {
            ++item.attempts;
        }
        return false;  // retry on the next trigger
    }

    // The other gate, and it is not a world condition: a `kImmediate` item is
    // waiting on a *position in the run*, not on the actor. Never charges an
    // attempt, because "the settle pass has not reached its last round yet" is not
    // a chance the item has had and wasted.
    if (item.WantsTrigger(Trigger::kImmediate) && !releaseImmediate) {
        return false;
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

    // Past the gate: from here the item touches the world, which is what the
    // per-frame budget measures. Set before the apply rather than after, so an
    // applier that throws still costs its slot.
    didWork = true;

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
    } catch (...) {
        // A non-std exception escaping into the engine's frame loop is a crash
        // with no log line naming the category that caused it.
        sink.Failed(subjectRef, item.subjectFormKey, Report::ReasonCode::kIoError,
                    "deferred applier threw a non-std exception");
        retire = true;
    }
    sink.EndCategory();

    if (!retire && countsAsAttempt) {
        ++item.attempts;
    }
    return retire;
}

void DeferredRestoreManager::Drain() {
    std::vector<ReadySignal> ready;
    {
        std::lock_guard lock(m_readyMutex);
        ready.swap(m_ready);
    }
    if (ready.empty() || PendingWorkQueue::Get().Empty()) {
        // The emptiness check is before the sink, not inside `DrainPass`: creating
        // the supplement sink for a drain with nothing to drain would arm
        // `WriteSupplement` to eventually emit an empty report.
        return;
    }

    // The supplement sink, shared across every event-driven drain, so the report it
    // eventually writes describes the whole deferred lifetime rather than one pass.
    std::shared_ptr<Report::ReportSink> sink;
    {
        auto* calendar = RE::Calendar::GetSingleton();
        const float gameDays = calendar ? calendar->GetDaysPassed() : 0.0f;
        std::lock_guard lock(m_supplementMutex);
        if (!m_supplementSink) {
            m_supplementSink = std::make_shared<Report::ReportSink>();
            m_supplementSink->SetHeader("import", "", "", "", "", gameDays, 0);
        }
        sink = m_supplementSink;
    }

    DrainPass(ready, *sink, /*isSupplement=*/true);
}

DeferredRestoreManager::DrainOutcome DeferredRestoreManager::DrainNow(Report::ReportSink& sink,
                                                                     bool releaseImmediate) {
    // A synthetic signal rather than whatever the sinks happen to have observed:
    // the settle pass is not reacting to a world event, it is asserting "now is the
    // moment, test everything". No attempt is charged for the same reason the load
    // kick charges none - the subject may be nowhere near the player and never had
    // a chance to be ready.
    const std::vector<ReadySignal> ready{ReadySignal{std::string{}, 0xFF, /*matchAll=*/true,
                                                     /*countsAsAttempt=*/false, releaseImmediate}};
    return DrainPass(ready, sink, /*isSupplement=*/false);
}

DeferredRestoreManager::DrainOutcome DeferredRestoreManager::DrainPass(
    const std::vector<ReadySignal>& ready, Report::ReportSink& sink, bool isSupplement) {
    DrainOutcome outcome;

    auto& queue = PendingWorkQueue::Get();
    uint64_t generationAtStart = 0;
    auto items = queue.Items(generationAtStart);
    if (items.empty()) {
        return outcome;
    }

    // An empty key from the load/force path means "consider everything".
    //
    // `countsAsAttempt` is OR-ed rather than AND-ed across the signals that
    // match an item: if a real world event named this subject in the same frame
    // as the blanket load re-test, the subject really was there and really was
    // not ready, so the attempt is earned.
    uint8_t globalTriggers = 0;
    bool globalMatchAll = false;
    bool globalCounts = false;
    // Not per key: releasing the immediate items is a statement about the run's
    // position, not about any one subject, so any signal asking for it asks for all
    // of them.
    bool releaseImmediate = false;
    std::unordered_map<std::string, std::pair<uint8_t, bool>> perKey;
    for (const auto& signal : ready) {
        releaseImmediate |= signal.releaseImmediate;
        if (signal.key.empty()) {
            globalTriggers |= signal.triggers;
            globalMatchAll |= signal.matchAll;
            globalCounts |= signal.countsAsAttempt;
        } else {
            auto& slot = perKey[signal.key];
            slot.first |= signal.triggers;
            slot.second |= signal.countsAsAttempt;
        }
    }

    // See `kMaxAppliesPerDrain` for why this is its own number and not
    // `iItemsPerFrame`. Whatever is left over is re-signalled and picked up on
    // the next frame.
    constexpr uint32_t budget = kMaxAppliesPerDrain;
    uint32_t worked = 0;
    bool deferredRemainder = false;

    auto* calendar = RE::Calendar::GetSingleton();
    const float gameDays = calendar ? calendar->GetDaysPassed() : 0.0f;

    std::vector<PendingItem> survivors;
    survivors.reserve(items.size());
    uint32_t retired = 0;

    // Every item this pass actually looked at, so `CommitDrain` can tell an item
    // an applier enqueued *while the drain was running* - which must be kept -
    // from a re-queue of one the drain just retired, which must not, because
    // honouring that would reset its attempt counter and never terminate.
    std::vector<std::pair<std::string, std::string>> processed;
    processed.reserve(items.size());

    for (auto& item : items) {
        if (worked >= budget) {
            // Budget spent. Everything untried is carried over untouched - not
            // recorded as processed, because nothing about it was decided.
            deferredRemainder = true;
            survivors.push_back(std::move(item));
            continue;
        }

        uint8_t triggers = globalTriggers;
        bool counts = globalCounts;
        if (const auto it = perKey.find(item.subjectFormKey); it != perKey.end()) {
            triggers |= it->second.first;
            counts |= it->second.second;
        }
        if (const auto it = perKey.find(item.cellFormKey); it != perKey.end()) {
            triggers |= it->second.first;
            counts |= it->second.second;
        }
        if (!globalMatchAll && (triggers & item.trigger) == 0) {
            // Not reached, so deliberately not recorded as processed: nothing
            // about it was decided this pass.
            survivors.push_back(std::move(item));
            continue;
        }
        processed.emplace_back(item.categoryId, item.subjectFormKey);

        // Expiry before work: a stale item should not touch the world.
        if (item.expiresAtGameTime > 0.0f && gameDays > item.expiresAtGameTime) {
            sink.BeginCategory(item.categoryId, item.categoryId, 0);
            sink.Failed(Report::SystemSubject(item.subjectFormKey), item.subjectFormKey,
                        Report::ReasonCode::kDeferredExpired,
                        std::format("passed its game-day deadline of {:.2f}",
                                    item.expiresAtGameTime));
            sink.EndCategory();
            ++retired;
            m_anythingRetired = true;
            continue;
        }

        bool didWork = false;
        const bool retire = ApplyItem(item, sink, counts, releaseImmediate, didWork);
        if (didWork) {
            ++worked;
        }
        if (retire) {
            ++retired;
            m_anythingRetired = true;
            continue;
        }

        if (item.attempts >= item.maxAttempts) {
            sink.BeginCategory(item.categoryId, item.categoryId, 0);
            sink.Failed(Report::SystemSubject(item.subjectFormKey), item.subjectFormKey,
                        Report::ReasonCode::kDeferredExhausted,
                        std::format("{} attempt(s) without the subject becoming ready",
                                    item.attempts));
            sink.EndCategory();
            ++retired;
            m_anythingRetired = true;
            continue;
        }
        survivors.push_back(std::move(item));
    }

    // Not `Replace`: an applier can enqueue while the drain is running, and that
    // work lives in the live queue rather than in `survivors`, which was built
    // from a copy taken before the pass began.
    queue.CommitDrain(std::move(survivors), processed, generationAtStart);

    outcome.retired = retired;
    outcome.worked = worked;
    outcome.remaining = queue.Size();
    outcome.budgetHit = deferredRemainder;

    if (deferredRemainder && isSupplement) {
        // Re-arm for the next frame. `matchAll` so the carried-over items are
        // reconsidered without needing the world event to happen again, and no
        // attempt charged, because they were never tried.
        //
        // Only on the event path: a settle-pass drain reports `budgetHit` to its
        // caller instead, which is already running rounds and would otherwise get a
        // second, competing drain scheduled behind it - writing into the supplement
        // sink rather than into the import report.
        spdlog::debug("DeferredRestoreManager: budget of {} reached, {} item(s) carried to the "
                      "next frame",
                      budget, outcome.remaining);
        Signal(ReadySignal{std::string{}, 0xFF, /*matchAll=*/true, /*countsAsAttempt=*/false,
                           /*releaseImmediate=*/releaseImmediate});
    }

    if (retired > 0) {
        spdlog::info("DeferredRestoreManager: retired {} item(s), {} remaining", retired,
                     outcome.remaining);
    }

    if (isSupplement && queue.Empty() && m_anythingRetired) {
        // Queue empty -> sinks go inert (they short-circuit on Empty()) and the
        // supplement report is written. Never from the settle pass: its lines went
        // into the import report, and a supplement naming the same items again
        // would read as a second, later round of work that never happened.
        WriteSupplement();
    }
    return outcome;
}

uint32_t DeferredRestoreManager::ReportRemaining(Report::ReportSink& sink) {
    const auto items = PendingWorkQueue::Get().Items();
    if (items.empty()) {
        return 0;
    }

    for (const auto& item : items) {
        // The subject is named from the live actor where it resolves, because
        // "Lydia" is what the player can act on and the form key is not. An
        // unresolvable one still gets a line: it is on the queue either way, and
        // the drain is where it gets retired for being unresolvable.
        Report::ReasonCode reason = Report::ReasonCode::kNone;
        auto* actor = Model::FormResolver::Get().ResolveChecked<RE::Actor>(item.subjectFormKey,
                                                                          reason);
        const char* name = actor ? actor->GetName() : nullptr;
        const auto displayName = (name && *name) ? Util::ConvertSkyrimTextToUTF8(name)
                                                 : item.subjectFormKey;

        // Resumes the row the apply pass already opened for this category, which is
        // where these lines belong. The name and phase are only consulted when no
        // row exists - a queue item from a session whose build had a category this
        // one does not - so they are looked up rather than invented.
        auto* category = Core::CategoryRegistry::Get().FindActor(item.categoryId);
        std::string_view displayNameForRow = item.categoryId;
        int phaseForRow = Core::PhaseValue(Core::Phase::kSettle);
        if (category) {
            const auto& descriptor = category->Describe();
            displayNameForRow = descriptor.displayName;
            phaseForRow = Core::PhaseValue(descriptor.phase);
        }
        sink.BeginCategory(item.categoryId, displayNameForRow, phaseForRow);
        sink.Deferred(Report::SubjectRef{Report::SubjectKind::kActor, item.subjectFormKey,
                                        displayName},
                      std::format("{}/{}", item.subjectFormKey, item.categoryId),
                      std::format("'{}' was not reachable during the import - {}", displayName,
                                  RemainingNotice(item.categoryId)));
        sink.EndCategory();
    }
    return static_cast<uint32_t>(items.size());
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
