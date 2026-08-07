#include "categories/EquipmentCommon.h"

#include <format>

#include "model/FormRef.h"
#include "util/ActorEnum.h"
#include "util/InventoryCount.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;

/// All 32 biped slots, as the engine numbers them. Reading every slot rather than
/// only the hands matters because an NPC's worn armour is what OBody's clothing
/// morphs are computed from, and slot 32 (kBody) in particular drives ORefit.
constexpr Slot kBipedSlots[] = {
    Slot::kHead,              Slot::kHair,           Slot::kBody,
    Slot::kHands,             Slot::kForearms,       Slot::kAmulet,
    Slot::kRing,              Slot::kFeet,           Slot::kCalves,
    Slot::kShield,            Slot::kTail,           Slot::kLongHair,
    Slot::kCirclet,           Slot::kEars,           Slot::kModMouth,
    Slot::kModNeck,           Slot::kModChestPrimary, Slot::kModBack,
    Slot::kModMisc1,          Slot::kModPelvisPrimary, Slot::kDecapitateHead,
    Slot::kDecapitate,        Slot::kModPelvisSecondary, Slot::kModLegRight,
    Slot::kModLegLeft,        Slot::kModFaceJewelry, Slot::kModChestSecondary,
    Slot::kModShoulder,       Slot::kModArmLeft,     Slot::kModArmRight,
    Slot::kModMisc2,          Slot::kFX01,
};

std::string NameOf(RE::TESForm* form) {
    if (!form) {
        return "";
    }
    const char* name = form->GetName();
    return (name && *name) ? Util::ConvertSkyrimTextToUTF8(name) : "";
}

}  // namespace

nlohmann::json EquipmentCommon::Collect(RE::Actor* actor) {
    auto worn = nlohmann::json::array();
    if (!actor) {
        return worn;
    }

    // Deduplicate: one armour piece commonly occupies several biped slots, and
    // equipping it twice is at best wasted work.
    std::vector<RE::TESForm*> seen;
    const auto record = [&](RE::TESForm* form, std::string_view slotName) {
        if (!form) {
            return;
        }
        if (std::find(seen.begin(), seen.end(), form) != seen.end()) {
            return;
        }
        const auto key = Model::FormKeyUtil::BuildFormKey(form);
        if (key.empty()) {
            return;  // dynamic: covered by the inventory reconstruct path
        }
        seen.push_back(form);
        worn.push_back({
            {"form", key},
            {"displayName", NameOf(form)},
            {"slot", std::string(slotName)},
        });
    };

    for (const auto slot : kBipedSlots) {
        record(actor->GetWornArmor(slot), "biped");
    }
    record(actor->GetEquippedObject(false), "right");
    record(actor->GetEquippedObject(true), "left");

    return worn;
}

EquipmentCommon::ApplyResult EquipmentCommon::Apply(RE::Actor* actor, const nlohmann::json& worn,
                                                   const Report::SubjectRef& subject,
                                                   Core::ApplyContext& ctx) {
    ApplyResult result;
    if (!actor || !worn.is_array()) {
        return result;
    }

    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) {
        ctx.report.Failed(subject, std::format("{}/equip", subject.formKey),
                          Report::ReasonCode::kIoError, "ActorEquipManager unavailable");
        return result;
    }

    auto& resolver = Model::FormResolver::Get();

    for (const auto& entry : worn) {
        const auto key = entry.value("form", std::string{});
        const auto displayName = entry.value("displayName", std::string{});
        if (key.empty()) {
            continue;
        }
        const auto itemId = std::format("{}/equip/{}", subject.formKey, key);

        Report::ReasonCode reason = Report::ReasonCode::kNone;
        auto* object = resolver.ResolveChecked<RE::TESBoundObject>(key, reason);
        if (!object) {
            ++result.failed;
            ctx.report.Failed(subject, itemId, reason,
                              std::format("'{}' could not be resolved to equip", displayName), key,
                              displayName);
            continue;
        }

        // The item must be in the container. If the inventory phase failed to add
        // it, equipping would silently do nothing, so say so instead.
        if (Util::CountInInventory(actor, object) <= 0) {
            ++result.failed;
            ctx.report.Failed(subject, itemId, Report::ReasonCode::kOutfitItemSkipped,
                              std::format("'{}' is not in the inventory, so it was not equipped",
                                          displayName),
                              key, displayName);
            continue;
        }

        equipManager->EquipObject(actor, object, nullptr, 1, nullptr, /*queueEquip=*/true,
                                  /*forceEquip=*/false, /*playSounds=*/false, /*applyNow=*/false);
        ++result.equipped;
        ctx.report.Succeeded(subject, itemId, key, displayName);
    }

    return result;
}

uint32_t EquipmentCommon::UnequipAllWorn(RE::Actor* actor) {
    if (!actor) {
        return 0;
    }
    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) {
        return 0;
    }

    std::vector<RE::TESBoundObject*> toRemove;
    for (const auto slot : kBipedSlots) {
        if (auto* armor = actor->GetWornArmor(slot)) {
            if (std::find(toRemove.begin(), toRemove.end(),
                          static_cast<RE::TESBoundObject*>(armor)) == toRemove.end()) {
                toRemove.push_back(armor);
            }
        }
    }

    uint32_t removed = 0;
    for (auto* object : toRemove) {
        equipManager->UnequipObject(actor, object, nullptr, 1, nullptr, true, false, false, false);
        ++removed;
    }
    return removed;
}

}  // namespace SaveMigration::Categories
