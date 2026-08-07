#include "model/WellKnownForms.h"

#include <format>
#include <vector>

#include "config/ConfigStorage.h"
#include "model/FormRef.h"
#include "util/StringUtil.h"

namespace SaveMigration::Model {

namespace {

/// Documented vanilla form IDs. Local == runtime for Skyrim.esm, whose compile
/// index is always 0x00.
///
/// VERIFIED against decompiled sources in this workspace (the Papyrus compiler
/// emits `[FACT:xxxxxxxx]` annotations next to property declarations):
///   CurrentFollowerFaction - `papyrus/reference/DOM/pahslave.psc:15`, and
///   `phhshinterface.psc:97` does the same GetFormFromFile call we do.
///
/// DOCUMENTED-ONLY, i.e. taken from the usual community references and not
/// cross-checked here: the dismissed/potential follower factions, the
/// DialogueFollower quest, PlayerFaction and Gold001. Each is verified at
/// runtime by form type, is overridable from the INI, and its failure degrades a
/// single roster source to "found nothing" plus one report line - so a wrong
/// value here cannot corrupt a migration, only narrow it.
/// The three faction IDs below and PlayerFaction were corrected on 2026-08-07 from
/// values observed in a running game: each was resolved by `LookupByEditorID`, which
/// warned that the previously documented ID disagreed. The dismissed/potential pair
/// was simply transposed. All four resolve to a load-order byte of 0x00, i.e. they are
/// Skyrim.esm base records whose FormIDs no plugin can move, so the runtime reading is
/// authoritative rather than load-order specific.
constexpr RE::FormID kCurrentFollowerFaction = 0x0005C84E;
constexpr RE::FormID kDismissedFollowerFaction = 0x0005C84C;
constexpr RE::FormID kPotentialFollowerFaction = 0x0005C84D;
constexpr RE::FormID kDialogueFollowerQuest = 0x000750BA;
constexpr RE::FormID kPlayerFaction = 0x00000DB1;
constexpr RE::FormID kGold001 = 0x0000000F;

std::string OverrideKey(std::string_view label) {
    return std::format("WellKnown:s{}", label);
}

}  // namespace

WellKnownForms& WellKnownForms::Get() {
    static WellKnownForms instance;
    return instance;
}

RE::TESForm* WellKnownForms::ResolveRaw(std::string_view label, std::string_view editorId,
                                       RE::FormID vanillaFormId) {
    // 1. INI override wins outright - it is the escape hatch when a load order
    //    genuinely moves one of these.
    const auto override = ::Config::ConfigStorage::GetSingleton()->GetString(OverrideKey(label), "");
    if (!override.empty()) {
        if (auto* form = FormKeyUtil::Resolve(override)) {
            spdlog::info("WellKnownForms: {} resolved from INI override '{}'", label, override);
            return form;
        }
        spdlog::warn("WellKnownForms: INI override '{}' for {} did not resolve", override, label);
    }

    // 2. Editor ID, when something in the load order retains them.
    if (!editorId.empty()) {
        if (auto* form = RE::TESForm::LookupByEditorID(editorId)) {
            if (form->GetFormID() != vanillaFormId) {
                spdlog::warn(
                    "WellKnownForms: {} found by editor ID at {:08X}, but the documented vanilla ID "
                    "is {:08X}. Using the editor-ID match.",
                    label, form->GetFormID(), vanillaFormId);
            }
            return form;
        }
    }

    // 3. Documented vanilla ID.
    return RE::TESForm::LookupByID(vanillaFormId);
}

template <class T>
T* WellKnownForms::ResolveOne(std::string_view label, std::string_view editorId,
                             RE::FormID vanillaFormId) {
    auto* raw = ResolveRaw(label, editorId, vanillaFormId);
    if (!raw) {
        spdlog::error("WellKnownForms: {} could not be resolved (vanilla {:08X})", label,
                      vanillaFormId);
        m_unresolved.emplace_back(label);
        return nullptr;
    }
    // Type-check rather than cast blindly: if a plugin replaced the record with
    // a different type, a blind cast is a crash on first use.
    auto* typed = raw->As<T>();
    if (!typed) {
        spdlog::error("WellKnownForms: {} at {:08X} is FormType {}, not the expected type", label,
                      raw->GetFormID(), static_cast<uint32_t>(raw->GetFormType()));
        m_unresolved.emplace_back(std::format("{} (wrong form type)", label));
        return nullptr;
    }
    spdlog::debug("WellKnownForms: {} -> {:08X}", label, typed->GetFormID());
    return typed;
}

void WellKnownForms::Resolve() {
    if (m_resolved) {
        return;
    }
    m_unresolved.clear();

    m_currentFollower = ResolveOne<RE::TESFaction>("CurrentFollowerFaction",
                                                   "CurrentFollowerFaction", kCurrentFollowerFaction);
    m_dismissedFollower = ResolveOne<RE::TESFaction>(
        "DismissedFollowerFaction", "DismissedFollowerFaction", kDismissedFollowerFaction);
    m_potentialFollower = ResolveOne<RE::TESFaction>(
        "PotentialFollowerFaction", "PotentialFollowerFaction", kPotentialFollowerFaction);
    m_dialogueFollower =
        ResolveOne<RE::TESQuest>("DialogueFollower", "DialogueFollower", kDialogueFollowerQuest);
    m_playerFaction = ResolveOne<RE::TESFaction>("PlayerFaction", "PlayerFaction", kPlayerFaction);
    m_gold = ResolveOne<RE::TESObjectMISC>("Gold001", "Gold001", kGold001);

    m_resolved = true;
    if (m_unresolved.empty()) {
        spdlog::info("WellKnownForms: all vanilla forms resolved");
    } else {
        spdlog::warn("WellKnownForms: {} form(s) unresolved; the affected roster sources will "
                     "contribute nothing",
                     m_unresolved.size());
    }
}

}  // namespace SaveMigration::Model
