#include "categories/npc/NpcHomeNff.h"

#include <format>

#include "model/FormRef.h"
#include "papyrus/ModProbe.h"
#include "papyrus/PapyrusInterface.h"
#include "papyrus/PapyrusVariableInterface.h"
#include "util/MoveRefTo.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.home_nff";
constexpr std::string_view kHomeScript = "nwsFollowerHomeScript";

/// 20 home bases. NFF's own `nwsHomeMarkers` is declared this long.
constexpr uint32_t kBaseCount = 20;

constexpr const char* kMarkerArrays[] = {"nwsHomeMarkers", "nwsWorkMarkers", "nwsPlayMarkers"};
constexpr const char* kMarkerKeys[] = {"home", "work", "play"};

}  // namespace

const Core::CategoryDescriptor& NpcHomeNff::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "NFF homes",
        .phase = Core::Phase::kIntegrationsHomes,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {.plugins = {},
                        .scriptNames = {std::string(kHomeScript)},
                        .dllNames = {}},
        .schemaVersion = 1,
    };
    return descriptor;
}

bool NpcHomeNff::ResolveHandles(Report::ReportSink& sink) {
    if (m_handles.valid) {
        return true;
    }
    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    m_handles.quest = vars->FindQuestByScriptName(std::string(kHomeScript));
    if (!m_handles.quest) {
        sink.SkipCategory(Report::ReasonCode::kModApiMissing,
                          "no quest carries nwsFollowerHomeScript; NFF homes are not usable");
        return false;
    }

    const auto faction = vars->GetVariable(m_handles.quest, std::string(kHomeScript), "nwsFF_HomeFac");
    if (faction.success) {
        if (const auto* formId = std::get_if<RE::FormID>(&faction.value)) {
            if (auto* form = RE::TESForm::LookupByID(*formId)) {
                m_handles.homeFaction = form->As<RE::TESFaction>();
            }
        }
    }
    if (!m_handles.homeFaction) {
        sink.SkipCategory(Report::ReasonCode::kModApiMissing,
                          "nwsFF_HomeFac could not be read. Residency is encoded entirely in that "
                          "faction's rank, so NFF homes were skipped rather than half-applied.");
        return false;
    }

    m_handles.valid = true;
    return true;
}

void NpcHomeNff::BeginCollect(Core::CollectContext& ctx) { ResolveHandles(ctx.report); }

void NpcHomeNff::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.actor || !m_handles.valid) {
        return;
    }
    if (!subject.actor->IsInFaction(m_handles.homeFaction)) {
        return;
    }

    // The rank *is* the base index. One integer carries the whole assignment, and
    // GetFactionRank works on an unloaded actor.
    const auto baseIndex = subject.actor->GetFactionRank(m_handles.homeFaction, false);
    if (baseIndex < 0) {
        return;
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["baseIndex"] = baseIndex;

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/nff_home", subject.refKey), subject.refKey,
        std::format("{} resides at NFF base {}", subject.displayName, baseIndex));
}

void NpcHomeNff::EndCollect(Core::CollectContext& ctx) {
    if (!m_handles.valid) {
        return;
    }
    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    auto& payload = ctx.Payload(kId, Describe().schemaVersion);

    if (const auto total = vars->GetInt(m_handles.quest, std::string(kHomeScript), "nwsBaseTotal")) {
        payload["nwsBaseTotal"] = *total;
    }

    // The three 20-long marker arrays, plus the marker positions themselves. The
    // arrays hold references; the positions are what actually place a home.
    auto bases = nlohmann::json::array();
    for (uint32_t base = 0; base < kBaseCount; ++base) {
        nlohmann::json entry{{"index", base}};
        for (size_t list = 0; list < std::size(kMarkerArrays); ++list) {
            const auto array = vars->GetArrayVariable(m_handles.quest, std::string(kHomeScript),
                                                      kMarkerArrays[list]);
            if (!array.success || base >= array.elements.size()) {
                continue;
            }
            const auto* formId = std::get_if<RE::FormID>(&array.elements[base]);
            if (!formId || *formId == 0) {
                continue;
            }
            auto* form = RE::TESForm::LookupByID(*formId);
            auto* marker = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!marker) {
                continue;
            }
            const auto position = marker->GetPosition();
            entry[kMarkerKeys[list]] = {
                {"marker", Model::FormKeyUtil::BuildFormKey(marker)},
                {"cell", Model::FormKeyUtil::BuildFormKey(marker->GetParentCell())},
                {"worldspace", Model::FormKeyUtil::BuildFormKey(marker->GetWorldspace())},
                {"x", position.x},
                {"y", position.y},
                {"z", position.z},
            };
        }
        if (entry.size() > 1) {
            bases.push_back(std::move(entry));
        }
    }
    payload["bases"] = std::move(bases);

    ctx.report.Succeeded(Report::SystemSubject("NFF homes"), "nff_bases", "",
                         std::format("{} base(s) with markers", payload["bases"].size()));
}

void NpcHomeNff::BeginApply(Core::ApplyContext& ctx) {
    if (!ResolveHandles(ctx.report)) {
        return;
    }

    const auto& payload = ctx.Payload(kId);
    if (payload.contains("nwsBaseTotal")) {
        // Stashed, deliberately not written yet - see EndApply.
        m_recordedBaseTotal = payload.value("nwsBaseTotal", -1);
    }

    // Marker positions first: a resident assigned to a base whose marker is still
    // unplaced would sandbox at the origin until the marker moved.
    if (m_basesWritten) {
        return;
    }
    m_basesWritten = true;

    const auto bases = payload.find("bases");
    if (bases == payload.end() || !bases->is_array()) {
        return;
    }

    auto& resolver = Model::FormResolver::Get();
    uint32_t moved = 0;
    for (const auto& entry : *bases) {
        for (const char* key : kMarkerKeys) {
            const auto slot = entry.find(key);
            if (slot == entry.end() || !slot->is_object()) {
                continue;
            }
            auto* marker = resolver.ResolveChecked<RE::TESObjectREFR>(
                slot->value("marker", std::string{}));
            if (!marker) {
                continue;
            }
            auto* cell = resolver.ResolveChecked<RE::TESObjectCELL>(
                slot->value("cell", std::string{}));
            auto* worldSpace = resolver.ResolveChecked<RE::TESWorldSpace>(
                slot->value("worldspace", std::string{}));
            if (!cell && !worldSpace) {
                continue;
            }
            const RE::NiPoint3 position{slot->value("x", 0.0f), slot->value("y", 0.0f),
                                        slot->value("z", 0.0f)};
            if (Util::MoveRefTo(marker, cell, worldSpace, position)) {
                ++moved;
            }
        }
    }
    ctx.report.Succeeded(Report::SystemSubject("NFF homes"), "nff_markers", "",
                         std::format("{} marker(s) placed", moved));
}

void NpcHomeNff::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (subject.isPlayer || !subject.actor || !m_handles.valid) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object() || !payload.contains("baseIndex")) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto itemId = std::format("{}/nff_home", subject.refKey);
    const int32_t baseIndex = payload.value("baseIndex", 0);

    // Dispatch the mod's own assignment rather than writing the faction rank
    // directly: SetFollowerHome also fills the resident alias and refreshes the MCM
    // list, neither of which a bare AddToFaction would do.
    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    const bool dispatched =
        papyrus->CallMethod(m_handles.quest, std::string(kHomeScript), "SetFollowerHome",
                            {baseIndex, static_cast<RE::Actor*>(subject.actor), false});
    if (!dispatched) {
        ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kPapyrusCallFailed,
                          std::format("could not dispatch SetFollowerHome for '{}'",
                                      subject.displayName));
        return;
    }

    ++m_assigned;
    ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                         std::format("{} -> NFF base {}", subject.displayName, baseIndex));
}

void NpcHomeNff::EndApply(Core::ApplyContext& ctx) {
    if (!m_handles.valid || m_recordedBaseTotal < 0) {
        return;
    }

    // Only now. AddFollowerHome early-returns when nwsBaseTotal == maxHomeSlots
    // (nwsFollowerHomeScript.psc:119), so writing the total before the residents
    // were assigned would make every one of those assignments a silent no-op.
    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    if (vars->SetVariable(m_handles.quest, std::string(kHomeScript), "nwsBaseTotal",
                          m_recordedBaseTotal, "Int")) {
        // Why this is written last is in the header and in the comment above; the
        // report says what happened, not how the code is arranged.
        ctx.report.Info(std::format("nwsBaseTotal set to {} after all {} resident assignment(s).",
                                    m_recordedBaseTotal, m_assigned));
    } else {
        ctx.report.Warn(Report::ReasonCode::kVmVariableNotFound,
                        "nwsBaseTotal could not be written; NFF's MCM base count may read low");
    }
    m_recordedBaseTotal = -1;
}

}  // namespace SaveMigration::Categories
