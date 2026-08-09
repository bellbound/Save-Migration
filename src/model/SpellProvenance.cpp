#include "model/SpellProvenance.h"

#include <array>

#include "model/FormKeyUtil.h"
#include "util/StringUtil.h"

namespace SaveMigration::Model {

namespace {

/// The five files every Skyrim install has. Deliberately not extended with
/// "official" Creation Club content: a CC plugin is as optional as any mod, and
/// its powers are handed out by its own quest exactly the same way.
constexpr std::array<std::string_view, 5> kMasters{
    "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm",
};

}  // namespace

SpellProvenance::SpellProvenance() {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        spdlog::error("SpellProvenance: no TESDataHandler; no spell will count as tome-taught");
        return;
    }

    for (auto* book : handler->GetFormArray<RE::TESObjectBOOK>()) {
        // `teaches` is a union - the same eight bytes are a spell pointer or a
        // skill index depending on the flag - so the flag test is not an
        // optimisation, it is what stops a skill book's ActorValue being read as
        // a pointer.
        if (!book || !book->TeachesSpell()) {
            continue;
        }
        if (auto* spell = book->data.teaches.spell) {
            m_taught.insert(spell);
        }
    }

    spdlog::info("SpellProvenance: {} spell(s) are taught by a tome somewhere in the load order",
                 m_taught.size());
}

const SpellProvenance& SpellProvenance::Get() {
    static const SpellProvenance instance;
    return instance;
}

bool SpellProvenance::IsTaughtByTome(RE::SpellItem* spell) const {
    return spell && m_taught.contains(spell);
}

bool SpellProvenance::IsVanilla(std::string_view formKey) {
    const auto parsed = FormKeyUtil::ParseFormKey(formKey);
    if (!parsed) {
        return false;
    }
    for (const auto& master : kMasters) {
        if (Util::IEquals(parsed->pluginName, master)) {
            return true;
        }
    }
    return false;
}

bool SpellProvenance::ShouldRestore(RE::SpellItem* spell, std::string_view formKey) const {
    if (!spell) {
        return false;
    }
    switch (spell->GetSpellType()) {
        case RE::MagicSystem::SpellType::kSpell:
            // A castable spell is the one shape that is always the player's:
            // something taught it to them, and if the teacher is gone the spell
            // is the only record left of it.
            return true;

        case RE::MagicSystem::SpellType::kAbility:
            // Never. An ability is passive and always granted by something that
            // will grant it again - a race, a perk, an enchantment, a quest, a
            // script. The standing stone is the one that looks like an exception
            // and is not: it is applied by its own pass in PlayerSpellsShouts,
            // from the standing-stone table, rather than through this list.
            return false;

        case RE::MagicSystem::SpellType::kPower:
        case RE::MagicSystem::SpellType::kLesserPower:
        case RE::MagicSystem::SpellType::kVoicePower:
            // Powers are the mixed bag. Beast Form, the racial once-a-days and
            // the standing-stone powers are progression; the mod menus bound to
            // a lesser power are not. A vanilla master or a tome is what
            // separates them, and re-adding a vanilla power the race grants
            // anyway is a no-op rather than a mistake.
            return IsVanilla(formKey) || IsTaughtByTome(spell);

        default:
            // Disease, poison, addiction. These are conditions, not possessions,
            // and carrying one into a new save is neither faithful nor wanted.
            return false;
    }
}

std::string SpellProvenance::RefusalReason(RE::SpellItem* spell, std::string_view formKey) const {
    if (!spell || ShouldRestore(spell, formKey)) {
        return {};
    }
    switch (spell->GetSpellType()) {
        case RE::MagicSystem::SpellType::kAbility:
            return "it is a passive ability, which is always granted by a race, perk, quest or "
                   "script rather than learned - whatever granted it here will grant it again";
        case RE::MagicSystem::SpellType::kPower:
        case RE::MagicSystem::SpellType::kLesserPower:
        case RE::MagicSystem::SpellType::kVoicePower:
            return "it is a power from a mod with no spell tome behind it, which is what a mod's "
                   "own menu or utility power looks like; that mod hands it out itself";
        default:
            return "it is a disease, poison or addiction rather than something the character knows";
    }
}

}  // namespace SaveMigration::Model
