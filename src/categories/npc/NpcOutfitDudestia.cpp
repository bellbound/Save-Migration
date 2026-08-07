#include "categories/npc/NpcOutfitDudestia.h"

#include <format>

#include "model/FormRef.h"
#include "papyrus/ModProbe.h"
#include "papyrus/PapyrusInterface.h"
#include "papyrus/PapyrusVariableInterface.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.outfit_dudestia";
constexpr std::string_view kSubjectScript = "DudestiaOutfitChangerSubject";
constexpr std::string_view kMainScript = "DudestiaOutfitChangerMain";

/// The thirteen armour slots the subject script tracks, in its own order.
constexpr const char* kSlotProperties[] = {
    "Slot30", "Slot31", "Slot32", "Slot33", "Slot34", "Slot35", "Slot37",
    "Slot38", "Slot39", "Slot41", "Slot42", "Slot43", "Slot44",
};

}  // namespace

const Core::CategoryDescriptor& NpcOutfitDudestia::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Dudestia outfits",
        .phase = Core::Phase::kIntegrationsOutfits,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {.plugins = {},
                        .scriptNames = {std::string(kSubjectScript)},
                        .dllNames = {}},
        .schemaVersion = 1,
    };
    return descriptor;
}

bool NpcOutfitDudestia::ResolveHandles(Report::ReportSink& sink) {
    if (m_handles.valid) {
        return true;
    }
    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();

    // The subject state lives on a ReferenceAlias-derived script, so the quest is
    // found via the *main* script and the aliases are walked from there.
    m_handles.quest = vars->FindQuestByScriptName(std::string(kMainScript));
    if (!m_handles.quest) {
        // Some builds put nothing on a quest-level script; fall back to whichever
        // quest owns the subject aliases.
        m_handles.quest = vars->FindQuestByScriptName(std::string(kSubjectScript));
    }
    if (!m_handles.quest) {
        sink.SkipCategory(Report::ReasonCode::kModApiMissing,
                          "no quest carries Dudestia's scripts; the mod is not usable");
        return false;
    }
    m_handles.valid = true;
    return true;
}

void NpcOutfitDudestia::BeginCollect(Core::CollectContext& ctx) { ResolveHandles(ctx.report); }

void NpcOutfitDudestia::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.actor || !m_handles.valid) {
        return;
    }

    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    const auto aliases = vars->EnumerateRefAliases(m_handles.quest);

    for (const auto& entry : aliases) {
        if (entry.actor != subject.actor) {
            continue;
        }

        auto& payload = ctx.ActorPayload(kId, subject.refKey);
        payload["aliasIndex"] = entry.index;

        auto slots = nlohmann::json::object();
        for (const char* property : kSlotProperties) {
            const auto value =
                vars->GetAliasVariable(entry.alias, std::string(kSubjectScript), property);
            if (!value.success) {
                continue;
            }
            if (const auto* formId = std::get_if<RE::FormID>(&value.value); formId && *formId != 0) {
                if (auto* form = RE::TESForm::LookupByID(*formId)) {
                    slots[property] = Model::FormKeyUtil::BuildFormKey(form);
                }
            }
        }
        payload["slots"] = std::move(slots);

        // EmptySlot is recorded per subject because slot tests compare against this
        // specific form rather than against None.
        const auto emptySlot =
            vars->GetAliasVariable(entry.alias, std::string(kSubjectScript), "EmptySlot");
        if (emptySlot.success) {
            if (const auto* formId = std::get_if<RE::FormID>(&emptySlot.value); formId && *formId) {
                if (auto* form = RE::TESForm::LookupByID(*formId)) {
                    payload["emptySlot"] = Model::FormKeyUtil::BuildFormKey(form);
                }
            }
        }

        const auto lock = vars->GetAliasVariable(entry.alias, std::string(kSubjectScript), "Lock");
        if (lock.success) {
            if (const auto* value = std::get_if<bool>(&lock.value)) {
                payload["lock"] = *value;
            }
        }

        ctx.report.Succeeded(
            Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
            std::format("{}/dudestia", subject.refKey), subject.refKey,
            std::format("{} ({} slot(s))", subject.displayName, payload["slots"].size()));
        return;
    }
}

void NpcOutfitDudestia::BeginApply(Core::ApplyContext& ctx) { ResolveHandles(ctx.report); }

void NpcOutfitDudestia::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (subject.isPlayer || !subject.actor || !m_handles.valid) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object() || !payload.contains("slots")) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto itemId = std::format("{}/dudestia", subject.refKey);

    auto* vars = Papyrus::PapyrusVariableInterface::GetSingleton();
    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();

    // Mirror FindEmpty() rather than calling AddSubject, which ends in
    // OpenInventory(true) and would pop a container UI per subject.
    const auto aliases = vars->EnumerateRefAliases(m_handles.quest);
    RE::BGSRefAlias* target = nullptr;
    for (const auto& entry : aliases) {
        if (entry.actor == subject.actor) {
            target = entry.alias;  // already a subject
            break;
        }
    }
    if (!target) {
        for (const auto& entry : aliases) {
            if (!entry.actor) {
                target = entry.alias;
                break;
            }
        }
        if (!target) {
            ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kModApiMissing,
                              std::format("Dudestia has no free subject alias for '{}'",
                                          subject.displayName));
            return;
        }
        // ForceRefTo has no CommonLib binding at all, hence CallAliasMethod. The
        // DudestiaDressUpSubject keyword is alias-attached, so it lands with the fill.
        if (!papyrus->CallAliasMethod(target, "ReferenceAlias", "ForceRefTo",
                                     {static_cast<RE::TESObjectREFR*>(subject.actor)})) {
            ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kPapyrusCallFailed,
                              std::format("ReferenceAlias.ForceRefTo failed for '{}'",
                                          subject.displayName));
            return;
        }
    }

    auto& resolver = Model::FormResolver::Get();

    // EmptySlot **first**. It is compared against that specific form, not against
    // None - so an unset value makes every slot test true and the mod tries to equip
    // None into all thirteen slots.
    const auto emptySlotKey = payload.value("emptySlot", std::string{});
    if (!emptySlotKey.empty()) {
        if (auto* emptySlot = resolver.ResolveChecked<RE::TESForm>(emptySlotKey)) {
            vars->SetAliasVariable(target, std::string(kSubjectScript), "EmptySlot",
                                   emptySlot->GetFormID(), "Armor");
        }
    } else {
        ctx.report.Warn(Report::ReasonCode::kModApiMissing,
                        std::format("no EmptySlot form was recorded for '{}'. Without it every slot "
                                    "test reads as filled and Dudestia would try to equip None, so "
                                    "the slot writes below were skipped.",
                                    subject.displayName));
        return;
    }

    // Then the slots.
    uint32_t applied = 0;
    const auto slots = payload.find("slots");
    for (const char* property : kSlotProperties) {
        const auto entry = slots->find(property);
        if (entry == slots->end() || !entry->is_string()) {
            continue;
        }
        auto* armor = resolver.ResolveChecked<RE::TESObjectARMO>(entry->get<std::string>());
        if (!armor) {
            continue;
        }
        if (vars->SetAliasVariable(target, std::string(kSubjectScript), property,
                                   armor->GetFormID(), "Armor")) {
            ++applied;
        }
    }

    // Lock last.
    if (payload.contains("lock")) {
        vars->SetAliasVariable(target, std::string(kSubjectScript), "Lock",
                              payload.value("lock", false), "Bool");
    }

    ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                         std::format("{} ({} slot(s) restored)", subject.displayName, applied));
    ctx.report.Info(
        "Dudestia subjects were installed by mirroring FindEmpty and ForceRefTo rather than by "
        "calling AddSubject, which ends in OpenInventory(true) and would pop one container window "
        "per subject. ChangeState and MakeNude were also avoided - they toggle and prompt.");
}

}  // namespace SaveMigration::Categories
