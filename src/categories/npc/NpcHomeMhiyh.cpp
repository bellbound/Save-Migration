#include "categories/npc/NpcHomeMhiyh.h"

#include <algorithm>
#include <format>

#include "defer/PendingWorkQueue.h"
#include "model/FormRef.h"
#include "papyrus/ModProbe.h"
#include "papyrus/PapyrusInterface.h"
#include "papyrus/PapyrusVariableInterface.h"
#include "util/GameThread.h"
#include "util/MoveRefTo.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.home_mhiyh";
constexpr std::string_view kQuestScript = "vvvMarkHomeQuest";

/// The seven per-slot marker lists, in the order the mod declares them. The JSON
/// key is stable; the property name is what we read off the quest script.
struct MarkerList {
    const char* jsonKey;
    const char* property;
};
constexpr std::array<MarkerList, 7> kMarkerLists = {{
    {"home", "vvvHomeMarkers"},
    {"guard", "vvvGuardMarkers"},
    {"guardDay", "vvvGuardDayMarkers"},
    {"sleep", "vvvSleepMarkers"},
    {"sleepDay", "vvvSleepDayMarkers"},
    {"work", "vvvWorkMarkers"},
    {"workNight", "vvvWorkNightMarkers"},
}};

/// `GetNumAliases() - 3`: MHIYH's own loop bound. The last three aliases are not
/// follower slots (the player plus two bookkeeping aliases), and treating them as
/// slots would fill the player into a home.
constexpr uint32_t kNonSlotAliases = 3;

nlohmann::json MarkerToJson(RE::TESObjectREFR* marker) {
    if (!marker) {
        return nlohmann::json();
    }
    const auto position = marker->GetPosition();
    return nlohmann::json{
        {"cell", Model::FormKeyUtil::BuildFormKey(marker->GetParentCell())},
        {"worldspace", Model::FormKeyUtil::BuildFormKey(marker->GetWorldspace())},
        {"x", position.x},
        {"y", position.y},
        {"z", position.z},
    };
}

RE::BGSListForm* ReadFormList(RE::TESQuest* quest, const char* property) {
    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    const auto result = vars->GetVariable(quest, std::string(kQuestScript), property);
    if (!result.success) {
        return nullptr;
    }
    if (const auto* formId = std::get_if<RE::FormID>(&result.value)) {
        auto* form = RE::TESForm::LookupByID(*formId);
        return form ? form->As<RE::BGSListForm>() : nullptr;
    }
    return nullptr;
}

RE::TESObjectREFR* NthOfList(RE::BGSListForm* list, uint32_t index) {
    if (!list || index >= list->forms.size()) {
        return nullptr;
    }
    auto* form = list->forms[index];
    return form ? form->As<RE::TESObjectREFR>() : nullptr;
}

RE::TESFaction* NthFaction(RE::BGSListForm* list, uint32_t index) {
    if (!list || index >= list->forms.size()) {
        return nullptr;
    }
    auto* form = list->forms[index];
    return form ? form->As<RE::TESFaction>() : nullptr;
}

}  // namespace

const Core::CategoryDescriptor& NpcHomeMhiyh::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "MHIYH homes",
        .phase = Core::Phase::kIntegrationsHomes,
        .restoreMode = Core::RestoreMode::kHybrid,
        .requirement = {.plugins = {std::string(Papyrus::Known::kMhiyhPlugin)},
                        .scriptNames = {std::string(kQuestScript)},
                        .dllNames = {}},
        .schemaVersion = 1,
    };
    return descriptor;
}

bool NpcHomeMhiyh::ResolveHandles(Report::ReportSink& sink) {
    if (m_handles.valid) {
        return true;
    }

    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    m_handles.quest = vars->FindQuestByScriptName(std::string(kQuestScript));
    if (!m_handles.quest) {
        sink.SkipCategory(Report::ReasonCode::kModApiMissing,
                          "no quest carries vvvMarkHomeQuest; MHIYH is not usable");
        return false;
    }

    for (size_t i = 0; i < kMarkerLists.size(); ++i) {
        m_handles.markerLists[i] = ReadFormList(m_handles.quest, kMarkerLists[i].property);
        if (!m_handles.markerLists[i]) {
            sink.SkipCategory(
                Report::ReasonCode::kModApiMissing,
                std::format("vvvMarkHomeQuest::{} could not be read; MHIYH homes were skipped "
                            "rather than half-applied",
                            kMarkerLists[i].property));
            return false;
        }
    }

    m_handles.markedFactions = ReadFormList(m_handles.quest, "vvvMarkedFactions");
    if (!m_handles.markedFactions) {
        sink.SkipCategory(Report::ReasonCode::kModApiMissing,
                          "vvvMarkedFactions could not be read; MHIYH homes were skipped");
        return false;
    }

    // The holding cell every unused marker sits in. Its identity *is* the mod's
    // occupancy test, so without it we cannot tell a set home from an unset one.
    const auto cellResult = vars->GetVariable(m_handles.quest, std::string(kQuestScript),
                                              "aaaMarkers");
    if (cellResult.success) {
        if (const auto* formId = std::get_if<RE::FormID>(&cellResult.value)) {
            if (auto* form = RE::TESForm::LookupByID(*formId)) {
                m_handles.holdingCell = form->As<RE::TESObjectCELL>();
            }
        }
    }
    if (!m_handles.holdingCell) {
        sink.SkipCategory(Report::ReasonCode::kModApiMissing,
                          "aaaMarkers (the marker holding cell) could not be read. Occupancy cannot "
                          "be determined without it, so MHIYH homes were skipped.");
        return false;
    }

    m_handles.valid = true;
    spdlog::info("NpcHomeMhiyh: quest {:08X}, {} slots", m_handles.quest->GetFormID(),
                 m_handles.markedFactions->forms.size());
    return true;
}

void NpcHomeMhiyh::BeginCollect(Core::CollectContext& ctx) { ResolveHandles(ctx.report); }

void NpcHomeMhiyh::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.actor || !m_handles.valid) {
        return;
    }

    const auto slotCount = static_cast<uint32_t>(m_handles.markedFactions->forms.size());
    for (uint32_t slot = 0; slot < slotCount; ++slot) {
        auto* faction = NthFaction(m_handles.markedFactions, slot);
        if (!faction || !subject.actor->IsInFaction(faction)) {
            continue;
        }
        // The mod's own occupancy test: a marker still parked in the holding cell
        // means this slot is not actually a set home.
        auto* homeMarker = NthOfList(m_handles.markerLists[0], slot);
        if (!homeMarker || homeMarker->GetParentCell() == m_handles.holdingCell) {
            continue;
        }

        auto& payload = ctx.ActorPayload(kId, subject.refKey);
        payload["slot"] = slot;
        payload["factionForm"] = Model::FormKeyUtil::BuildFormKey(faction);

        auto markers = nlohmann::json::object();
        for (size_t i = 0; i < kMarkerLists.size(); ++i) {
            if (auto* marker = NthOfList(m_handles.markerLists[i], slot)) {
                // A marker still in the holding cell is "not set for this role",
                // which is different from "set to the holding cell" - so it is
                // recorded as null and skipped on apply.
                if (marker->GetParentCell() != m_handles.holdingCell) {
                    markers[kMarkerLists[i].jsonKey] = MarkerToJson(marker);
                }
            }
        }
        payload["markers"] = std::move(markers);

        const char* cellName = homeMarker->GetParentCell()
                                   ? homeMarker->GetParentCell()->GetName()
                                   : nullptr;
        payload["homeCellName"] = (cellName && *cellName) ? Util::ConvertSkyrimTextToUTF8(cellName)
                                                         : "";

        ctx.report.Succeeded(
            Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
            std::format("{}/mhiyh", subject.refKey), subject.refKey,
            std::format("{} lives in {}", subject.displayName,
                        payload["homeCellName"].get<std::string>()));
        return;  // one home per actor
    }
}

void NpcHomeMhiyh::BeginApply(Core::ApplyContext& ctx) {
    if (!ResolveHandles(ctx.report) || m_queueBuilt || !ctx.subjects) {
        return;
    }
    m_queueBuilt = true;

    for (const auto& subject : *ctx.subjects) {
        if (subject.isPlayer || !subject.actor) {
            continue;
        }
        const auto& payload = ctx.ActorPayload(kId, subject.refKey);
        if (!payload.is_object() || !payload.contains("slot")) {
            continue;
        }
        m_pending.emplace_back(payload.value("slot", 0u), subject.refKey);
    }

    // Ascending, because ForceAlias fills the first empty alias: processing the
    // lowest recorded slot first is what keeps the compaction predictable.
    std::sort(m_pending.begin(), m_pending.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    if (!m_pending.empty()) {
        spdlog::info("NpcHomeMhiyh: {} home(s) to place", m_pending.size());
    }
}

void NpcHomeMhiyh::ApplyActor(const Model::ActorSubject&, Core::ApplyContext&) {
    // Nothing per actor: placement is driven from EndApply, one slot per frame,
    // because each ForceAlias restarts the Homies Book quest.
}

void NpcHomeMhiyh::EndApply(Core::ApplyContext& ctx) {
    if (!m_handles.valid || m_pending.empty()) {
        return;
    }

    const auto [recordedSlot, refKey] = m_pending.front();
    m_pending.erase(m_pending.begin());

    Report::ReasonCode reason = Report::ReasonCode::kNone;
    auto* actor = Model::FormResolver::Get().ResolveChecked<RE::Actor>(refKey, reason);
    const auto& payload = ctx.ActorPayload(kId, refKey);
    const char* rawName = actor ? actor->GetName() : nullptr;
    const std::string displayName = (rawName && *rawName) ? rawName : refKey;
    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, refKey, displayName};
    const auto itemId = std::format("{}/mhiyh", refKey);

    if (!actor) {
        ctx.report.Failed(subjectRef, itemId, reason, "MHIYH home subject could not be resolved");
        if (!m_pending.empty()) {
            ctx.RequestContinuation();
        }
        return;
    }

    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();

    // If this actor already holds an alias, no fill is needed - just re-place the
    // markers at whatever index they are actually in.
    const auto aliases = vars->EnumerateRefAliases(m_handles.quest);
    const uint32_t slotLimit =
        aliases.size() > kNonSlotAliases ? static_cast<uint32_t>(aliases.size() - kNonSlotAliases)
                                         : 0;

    std::optional<uint32_t> existingIndex;
    bool loadedOnly = false;
    for (const auto& entry : aliases) {
        if (entry.index >= slotLimit) {
            continue;  // one of the trailing non-slot aliases
        }
        if (entry.actor == actor) {
            existingIndex = entry.index;
            loadedOnly = entry.loadedOnly;
            break;
        }
    }

    // A kLoadedOnly alias will not fill for an unloaded actor - the call succeeds
    // and quietly does nothing. Defer instead of pretending it worked.
    if (!existingIndex && !actor->Is3DLoaded()) {
        bool anyLoadedOnly = false;
        for (const auto& entry : aliases) {
            if (entry.index < slotLimit && entry.loadedOnly && !entry.actor) {
                anyLoadedOnly = true;
                break;
            }
        }
        if (anyLoadedOnly) {
            Defer::PendingItem item;
            item.categoryId = std::string(kId);
            item.subjectFormKey = refKey;
            item.trigger = Defer::TriggerBits(Defer::Trigger::kActorLoaded);
            item.maxAttempts = 8;
            item.payload = Util::SafeDump(payload);
            if (ctx.pending.Enqueue(std::move(item))) {
                ctx.report.Deferred(subjectRef, itemId,
                                    std::format("'{}' is unloaded and the free MHIYH alias is "
                                                "kLoadedOnly, so the fill would silently fail. "
                                                "Queued until they load.",
                                                displayName));
                if (!m_pending.empty()) {
                    ctx.RequestContinuation();
                }
                return;
            }
        }
    }

    if (!existingIndex) {
        // Dispatch the mod's own fill. Native alias forcing is rejected - see the
        // class comment for the four things it would fail to do.
        if (!papyrus->CallMethod(m_handles.quest, std::string(kQuestScript), "ForceAlias",
                                 {static_cast<RE::TESObjectREFR*>(actor)})) {
            ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kPapyrusCallFailed,
                              std::format("could not dispatch vvvMarkHomeQuest.ForceAlias for '{}'",
                                          displayName));
            if (!m_pending.empty()) {
                ctx.RequestContinuation();
            }
            return;
        }
    }

    // The read-back. This is the whole point: ForceAlias filled the *first empty*
    // alias, so the index the actor is now in is authoritative and the recorded
    // slot is not. Runs next frame, because the dispatch is asynchronous.
    const auto markersJson = payload.value("markers", nlohmann::json::object());
    const auto factionKey = payload.value("factionForm", std::string{});
    auto handles = m_handles;
    Util::OnGameThread([handles, actor, refKey, displayName, recordedSlot, markersJson,
                        factionKey]() {
        auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
        const auto aliases = vars->EnumerateRefAliases(handles.quest);
        const uint32_t slotLimit = aliases.size() > kNonSlotAliases
                                       ? static_cast<uint32_t>(aliases.size() - kNonSlotAliases)
                                       : 0;

        std::optional<uint32_t> landedIndex;
        for (const auto& entry : aliases) {
            if (entry.index < slotLimit && entry.actor == actor) {
                landedIndex = entry.index;
                break;
            }
        }
        if (!landedIndex) {
            spdlog::warn(
                "NpcHomeMhiyh: '{}' is not in any MHIYH alias after ForceAlias; markers were not "
                "moved (recorded slot was {})",
                displayName, recordedSlot);
            return;
        }
        const uint32_t j = *landedIndex;
        if (j != recordedSlot) {
            spdlog::info(
                "NpcHomeMhiyh: '{}' was recorded in slot {} but landed in alias {} - markers go to "
                "{}, as ForceAlias compacts indices",
                displayName, recordedSlot, j, j);
        }

        // Move that slot's seven markers. MoveRefTo, not a teleport: this is why
        // home restore is instant instead of walking the player around the world.
        uint32_t moved = 0;
        for (size_t i = 0; i < kMarkerLists.size(); ++i) {
            const auto entry = markersJson.find(kMarkerLists[i].jsonKey);
            if (entry == markersJson.end() || !entry->is_object()) {
                continue;
            }
            auto* marker = NthOfList(handles.markerLists[i], j);
            if (!marker) {
                continue;
            }
            auto& resolver = Model::FormResolver::Get();
            auto* cell = resolver.ResolveChecked<RE::TESObjectCELL>(
                entry->value("cell", std::string{}));
            auto* worldSpace = resolver.ResolveChecked<RE::TESWorldSpace>(
                entry->value("worldspace", std::string{}));
            if (!cell && !worldSpace) {
                continue;
            }
            const RE::NiPoint3 position{entry->value("x", 0.0f), entry->value("y", 0.0f),
                                        entry->value("z", 0.0f)};
            if (Util::MoveRefTo(marker, cell, worldSpace, position)) {
                ++moved;
            }
        }

        // Verify the alias's attached faction actually landed. MHIYH relies on the
        // engine applying it at fill time; if that did not happen, add it directly.
        auto* faction = Model::FormResolver::Get().ResolveChecked<RE::TESFaction>(factionKey);
        auto* slotFaction = NthFaction(handles.markedFactions, j);
        auto* wanted = slotFaction ? slotFaction : faction;
        if (wanted && !actor->IsInFaction(wanted)) {
            actor->AddToFaction(wanted, 0);
            spdlog::info(
                "NpcHomeMhiyh: alias fill did not apply slot {}'s faction to '{}'; added it "
                "directly",
                j, displayName);
        }

        spdlog::info("NpcHomeMhiyh: '{}' homed at alias {} with {} marker(s) moved", displayName, j,
                     moved);
    });

    ++m_placed;
    ctx.report.Succeeded(subjectRef, itemId, refKey,
                         std::format("{} (home placed)", displayName));

    if (!m_pending.empty()) {
        // One per frame: each ForceAlias restarts the Homies Book quest, and a burst
        // of quest restarts in one frame is a stutter and a script-latency spike.
        ctx.RequestContinuation();
        return;
    }
    ctx.report.Info(std::format("{} MHIYH home(s) placed. Indices were read back after each fill "
                                "rather than trusted, because ForceAlias compacts them.",
                                m_placed));
}

bool NpcHomeMhiyh::ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (!ResolveHandles(ctx.report)) {
        return true;
    }
    if (!subject.actor || !subject.actor->Is3DLoaded()) {
        return false;  // still not loaded: retry
    }

    // A deferred replay can land *between two frames of this category's own
    // continuation loop*: the apply pass places one home per frame, and each
    // `ForceAlias` restarts a quest, which loads and unloads actors, which is
    // exactly what fires the object-load event that schedules a drain. Seeding
    // `m_pending` with this one subject and leaving it that way would then throw
    // away every home the loop had not reached yet - silently, since the loop
    // simply finds an empty queue on its next frame and reports itself complete.
    //
    // So the shared state is borrowed and handed straight back.
    auto savedPending = std::move(m_pending);
    const bool savedQueueBuilt = m_queueBuilt;

    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    m_pending.clear();
    m_pending.emplace_back(payload.value("slot", 0u), subject.refKey);
    m_queueBuilt = true;
    EndApply(ctx);

    m_pending = std::move(savedPending);
    m_queueBuilt = savedQueueBuilt;
    return true;
}

}  // namespace SaveMigration::Categories
