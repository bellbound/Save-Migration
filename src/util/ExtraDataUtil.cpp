#include "util/ExtraDataUtil.h"

#include <format>

#include "model/FormRef.h"
#include "util/StringUtil.h"

namespace SaveMigration::Util {

std::optional<float> ExtraDataUtil::ReadTemper(RE::ExtraDataList* extraList) {
    if (!extraList) {
        return std::nullopt;
    }
    if (auto* health = extraList->GetByType<RE::ExtraHealth>()) {
        return health->health;
    }
    return std::nullopt;
}

nlohmann::json ExtraDataUtil::DescribeEnchantment(RE::EnchantmentItem* enchantment) {
    if (!enchantment) {
        return nlohmann::json();
    }

    auto effects = nlohmann::json::array();
    for (const auto* effect : enchantment->effects) {
        if (!effect || !effect->baseEffect) {
            continue;
        }
        const char* name = effect->baseEffect->GetFullName();
        effects.push_back({
            {"effect", Model::FormKeyUtil::BuildFormKey(effect->baseEffect)},
            {"effectName", (name && *name) ? ConvertSkyrimTextToUTF8(name) : ""},
            {"magnitude", effect->effectItem.magnitude},
            {"area", effect->effectItem.area},
            {"duration", effect->effectItem.duration},
        });
    }

    const char* enchName = enchantment->GetFullName();
    return nlohmann::json{
        {"name", (enchName && *enchName) ? ConvertSkyrimTextToUTF8(enchName) : ""},
        {"isDynamic", Model::FormKeyUtil::BuildFormKey(enchantment).empty()},
        {"effects", std::move(effects)},
        {"note",
         "A player-made enchantment cannot be recreated - there is no API to mint one. This recipe "
         "is recorded so the item can be re-enchanted deliberately."},
    };
}

Model::ItemExtra ExtraDataUtil::Read(RE::ExtraDataList* extraList, RE::TESBoundObject* object) {
    Model::ItemExtra extra;
    if (!extraList) {
        return extra;
    }

    if (auto* enchantment = extraList->GetByType<RE::ExtraEnchantment>()) {
        // Only a plugin-backed enchantment gets a key. A dynamic one yields an
        // empty string, and the caller then routes the whole item to
        // `unmigratable[]` with a reconstruct block.
        extra.enchantmentKey = Model::FormKeyUtil::BuildFormKey(enchantment->enchantment);
        extra.charge = static_cast<float>(enchantment->charge);
    }
    if (auto* charge = extraList->GetByType<RE::ExtraCharge>()) {
        extra.charge = charge->charge;
    }
    if (auto* health = extraList->GetByType<RE::ExtraHealth>()) {
        extra.health = health->health;
    }
    if (auto* soul = extraList->GetByType<RE::ExtraSoul>()) {
        extra.soulLevel = static_cast<int>(soul->GetContainedSoul());
    }
    if (auto* poison = extraList->GetByType<RE::ExtraPoison>()) {
        extra.poisonKey = Model::FormKeyUtil::BuildFormKey(poison->poison);
        extra.poisonCount = static_cast<int>(poison->count);
    }
    if (auto* text = extraList->GetByType<RE::ExtraTextDisplayData>()) {
        // Only a *player-set* name is worth carrying. The engine also parks
        // generated names here (tempered prefixes), and re-applying one of those
        // would stack "Fine Fine Steel Sword" on the next temper.
        if (text->IsPlayerSet() && !text->displayName.empty()) {
            extra.displayName = ConvertSkyrimTextToUTF8(text->displayName.c_str());
        }
    }

    (void)object;  // reserved for future per-type handling
    return extra;
}

bool ExtraDataUtil::IsQuestItem(RE::InventoryEntryData* entry) {
    return entry && entry->IsQuestObject();
}

RE::ExtraDataList* ExtraDataUtil::FindOrCreateExtraList(RE::TESObjectREFR* container,
                                                        RE::TESBoundObject* object) {
    if (!container || !object) {
        return nullptr;
    }
    auto* changes = container->GetInventoryChanges();
    if (!changes || !changes->entryList) {
        return nullptr;
    }

    for (auto* entry : *changes->entryList) {
        if (!entry || entry->object != object) {
            continue;
        }
        if (entry->extraLists) {
            for (auto* list : *entry->extraLists) {
                if (list) {
                    return list;
                }
            }
        }
        // The stack exists but carries no extra list. Creating one here would
        // decorate the *whole* stack rather than one instance, which is wrong for
        // a count > 1 - so the caller adds single-count stacks for decorated
        // items and we simply report nothing to attach to.
        return nullptr;
    }
    return nullptr;
}

uint32_t ExtraDataUtil::Apply(RE::TESObjectREFR* container, RE::TESBoundObject* object,
                             const Model::ItemExtra& extra) {
    if (extra.IsEmpty()) {
        return 0;
    }
    auto* extraList = FindOrCreateExtraList(container, object);
    if (!extraList) {
        spdlog::debug("ExtraDataUtil: no extra list to decorate for {:08X}", object->GetFormID());
        return 0;
    }

    uint32_t applied = 0;

    if (!extra.enchantmentKey.empty()) {
        Report::ReasonCode reason = Report::ReasonCode::kNone;
        if (auto* enchantment = Model::FormResolver::Get().ResolveChecked<RE::EnchantmentItem>(
                extra.enchantmentKey, reason)) {
            if (!extraList->HasType<RE::ExtraEnchantment>()) {
                const auto charge =
                    static_cast<uint16_t>(std::clamp(extra.charge.value_or(0.0f), 0.0f, 65535.0f));
                extraList->Add(new RE::ExtraEnchantment(enchantment, charge, false));
                ++applied;
            }
        }
    }

    if (extra.health.has_value() && !extraList->HasType<RE::ExtraHealth>()) {
        // Temper. Applied before any value/armour-rating read so the engine
        // recomputes from the right base.
        extraList->Add(new RE::ExtraHealth(*extra.health));
        ++applied;
    }

    if (extra.charge.has_value() && !extraList->HasType<RE::ExtraCharge>() &&
        !extraList->HasType<RE::ExtraEnchantment>()) {
        auto* charge = new RE::ExtraCharge();
        charge->charge = *extra.charge;
        extraList->Add(charge);
        ++applied;
    }

    if (extra.soulLevel.has_value() && !extraList->HasType<RE::ExtraSoul>()) {
        const auto level = static_cast<RE::SOUL_LEVEL>(
            std::clamp(*extra.soulLevel, 0, static_cast<int>(RE::SOUL_LEVEL::kGrand)));
        extraList->Add(new RE::ExtraSoul(level));
        ++applied;
    }

    if (!extra.poisonKey.empty() && !extraList->HasType<RE::ExtraPoison>()) {
        Report::ReasonCode reason = Report::ReasonCode::kNone;
        if (auto* poison =
                Model::FormResolver::Get().ResolveChecked<RE::AlchemyItem>(extra.poisonKey, reason)) {
            extraList->Add(new RE::ExtraPoison(poison, extra.poisonCount.value_or(1)));
            ++applied;
        }
    }

    if (!extra.displayName.empty()) {
        if (auto* text = extraList->GetExtraTextDisplayData()) {
            text->SetName(extra.displayName.c_str());
            ++applied;
        } else {
            extraList->Add(new RE::ExtraTextDisplayData(extra.displayName.c_str()));
            ++applied;
        }
    }

    // ExtraUniqueID is deliberately never written: it is a per-save allocator
    // value, so carrying one over either collides with a live id or dangles.
    return applied;
}

}  // namespace SaveMigration::Util
