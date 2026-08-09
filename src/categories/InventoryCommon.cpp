#include "categories/InventoryCommon.h"

#include <algorithm>
#include <format>

#include "config/MigrationConfig.h"
#include "model/FormRef.h"
#include "model/WellKnownForms.h"
#include "util/ExtraDataUtil.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

std::string NameOf(RE::TESBoundObject* object) {
    if (!object) {
        return "";
    }
    const char* name = object->GetName();
    return (name && *name) ? Util::ConvertSkyrimTextToUTF8(name) : "";
}

/// Gold is handled by `player.currency` as an idempotent delta, so it must not
/// also arrive as an inventory line.
bool IsCurrency(RE::TESBoundObject* object) {
    auto* gold = Model::WellKnownForms::Get().Gold();
    return gold && object == gold;
}

}  // namespace

InventoryCommon::CollectResult InventoryCommon::Collect(RE::TESObjectREFR* container) {
    CollectResult result;
    if (!container) {
        return result;
    }

    auto* changes = container->GetInventoryChanges();
    const bool restoreQuestItems = Config::MigrationConfig::RestoreQuestItems();

    // Walk the inventory *changes* rather than GetInventory(): the changes list is
    // where the per-instance extra data lives, and without it a tempered,
    // enchanted or renamed item is indistinguishable from a plain one.
    if (changes && changes->entryList) {
        for (auto* entry : *changes->entryList) {
            if (!entry || !entry->object) {
                continue;
            }
            auto* object = entry->object;
            if (IsCurrency(object)) {
                continue;
            }
            const auto count = entry->countDelta;
            if (count <= 0) {
                continue;
            }

            const auto isQuestItem = entry->IsQuestObject();
            if (isQuestItem && !restoreQuestItems) {
                ++result.questItemsSkipped;
                result.unmigratable.push_back(Model::UnmigratableItem{
                    NameOf(object), "quest_item_skipped", count, {}}.ToJson());
                continue;
            }

            const auto key = Model::FormKeyUtil::BuildFormKey(object);
            const auto displayName = NameOf(object);

            // Gather the extras from the first extra list that has any. An entry
            // can hold several lists when instances differ; each becomes its own
            // single-count line so decoration is not smeared across a stack.
            std::vector<RE::ExtraDataList*> lists;
            if (entry->extraLists) {
                for (auto* list : *entry->extraLists) {
                    if (list) {
                        lists.push_back(list);
                    }
                }
            }

            if (key.empty()) {
                // A dynamic object - crafted or player-enchanted. It has no
                // cross-save identity, so record a reconstruct recipe instead.
                Model::UnmigratableItem item;
                item.displayName = displayName;
                item.reasonCode = "dynamic_form";
                item.count = count;
                // Best-effort base: a dynamic weapon/armour keeps its template.
                if (auto* weapon = object->As<RE::TESObjectWEAP>(); weapon && weapon->templateWeapon) {
                    item.reconstruct.baseKey =
                        Model::FormKeyUtil::BuildFormKey(weapon->templateWeapon);
                } else if (auto* armor = object->As<RE::TESObjectARMO>();
                           armor && armor->templateArmor) {
                    item.reconstruct.baseKey = Model::FormKeyUtil::BuildFormKey(armor->templateArmor);
                }
                if (!lists.empty()) {
                    item.reconstruct.health = Util::ExtraDataUtil::ReadTemper(lists.front());
                    const auto extra = Util::ExtraDataUtil::Read(lists.front(), object);
                    item.reconstruct.displayName = extra.displayName;
                }
                if (auto* enchantable = object->As<RE::TESEnchantableForm>();
                    enchantable && enchantable->formEnchanting) {
                    item.reconstruct.playerEnchantment =
                        Util::ExtraDataUtil::DescribeEnchantment(enchantable->formEnchanting);
                }
                result.unmigratable.push_back(item.ToJson());
                continue;
            }

            if (lists.empty()) {
                Model::ItemStack stack;
                stack.formKey = key;
                stack.count = count;
                stack.displayName = displayName;
                result.items.push_back(stack.ToJson());
                continue;
            }

            // One line per decorated instance, plus one for the undecorated
            // remainder if the counts do not add up.
            int32_t accountedFor = 0;
            for (auto* list : lists) {
                Model::ItemStack stack;
                stack.formKey = key;
                stack.count = 1;
                stack.displayName = displayName;
                stack.extra = Util::ExtraDataUtil::Read(list, object);
                result.items.push_back(stack.ToJson());
                ++accountedFor;
            }
            if (count > accountedFor) {
                Model::ItemStack stack;
                stack.formKey = key;
                stack.count = count - accountedFor;
                stack.displayName = displayName;
                result.items.push_back(stack.ToJson());
            }
        }
    }

    // Anything the changes list did not cover - base-record inventory that has
    // never been touched - comes from the plain count map.
    for (const auto& [object, data] : container->GetInventoryCounts()) {
        if (!object || IsCurrency(object)) {
            continue;
        }
        const auto key = Model::FormKeyUtil::BuildFormKey(object);
        if (key.empty()) {
            continue;
        }
        bool alreadyRecorded = false;
        for (const auto& recorded : result.items) {
            if (recorded.value("form", "") == key) {
                alreadyRecorded = true;
                break;
            }
        }
        if (alreadyRecorded) {
            continue;
        }
        Model::ItemStack stack;
        stack.formKey = key;
        stack.count = data;
        stack.displayName = NameOf(object);
        result.items.push_back(stack.ToJson());
    }

    return result;
}

bool InventoryCommon::ApplyChunk(RE::TESObjectREFR* container, const nlohmann::json& items,
                                 const Report::SubjectRef& subject, Core::ApplyContext& ctx,
                                 ApplyCursor& cursor, uint32_t maxThisFrame) {
    if (!container || !items.is_array()) {
        cursor.finished = true;
        return false;
    }

    auto& resolver = Model::FormResolver::Get();
    uint32_t thisFrame = 0;

    while (cursor.nextIndex < items.size() && thisFrame < maxThisFrame) {
        const auto& entry = items[cursor.nextIndex];
        ++cursor.nextIndex;
        ++thisFrame;

        const auto stack = Model::ItemStack::FromJson(entry);
        if (stack.formKey.empty() || stack.count <= 0) {
            // Counted rather than merely stepped over. `Validate` reconciles
            // added + failed + skipped against the recorded total and treats a
            // shortfall as "the walk stopped before the end", so an entry that
            // lands in none of the three buckets reads as a truncated import.
            //
            // Measured on Skyrim VR 1.4.15, 2026-08-09: this snapshot holds 8
            // count-0 stacks (Iron Shield, Iron Sword, Long Bow and the like),
            // and every one of them widened that gap. The restore itself was
            // clean - there is nothing to add for a count of zero - but the
            // import was still classified UNSAFE, which is the one verdict a
            // player is meant to act on.
            ++cursor.skipped;
            continue;
        }

        const auto itemId = std::format("{}/item/{}/{}", subject.formKey, stack.formKey,
                                        cursor.nextIndex);

        Report::ReasonCode reason = Report::ReasonCode::kNone;
        auto* object = resolver.ResolveChecked<RE::TESBoundObject>(stack.formKey, reason);
        if (!object) {
            ++cursor.failed;
            ctx.report.Failed(subject, itemId, reason,
                              std::format("'{}' x{} could not be resolved", stack.displayName,
                                          stack.count),
                              stack.formKey, stack.displayName);
            continue;
        }

        container->AddObjectToContainer(object, nullptr, stack.count, nullptr);
        // Decoration goes on after the add, because AddObjectToContainer takes no
        // extra list we can pre-build.
        Util::ExtraDataUtil::Apply(container, object, stack.extra);

        ++cursor.added;
        ctx.report.Succeeded(subject, itemId, stack.formKey, stack.displayName, stack.count);
    }

    cursor.finished = cursor.nextIndex >= items.size();
    return !cursor.finished;
}

void InventoryCommon::ApplyUnmigratable(RE::TESObjectREFR* container,
                                        const nlohmann::json& unmigratable,
                                        const Report::SubjectRef& subject,
                                        Core::ApplyContext& ctx) {
    if (!unmigratable.is_array()) {
        return;
    }

    const bool reconstruct = Config::MigrationConfig::ReconstructCraftedItems();
    auto& resolver = Model::FormResolver::Get();
    uint32_t rebuilt = 0;
    uint32_t reported = 0;

    for (const auto& entry : unmigratable) {
        const auto item = Model::UnmigratableItem::FromJson(entry);
        const auto itemId =
            std::format("{}/unmigratable/{}/{}", subject.formKey, item.displayName, reported);
        ++reported;

        if (item.reasonCode == "quest_item_skipped") {
            ctx.report.SkippedItem(subject, itemId, Report::ReasonCode::kQuestItemSkipped,
                                   std::format("'{}' x{} is a quest item", item.displayName,
                                               item.count),
                                   item.displayName);
            continue;
        }

        if (!reconstruct || item.reconstruct.baseKey.empty()) {
            ctx.report.Failed(subject, itemId, Report::ReasonCode::kDynamicForm,
                              std::format("'{}' x{} was a runtime-created object with no base "
                                          "record recorded, so nothing could be rebuilt",
                                          item.displayName, item.count),
                              "", item.displayName);
            continue;
        }

        Report::ReasonCode reason = Report::ReasonCode::kNone;
        auto* base = resolver.ResolveChecked<RE::TESBoundObject>(item.reconstruct.baseKey, reason);
        if (!base) {
            ctx.report.Failed(subject, itemId, reason,
                              std::format("'{}': base record '{}' could not be resolved",
                                          item.displayName, item.reconstruct.baseKey),
                              item.reconstruct.baseKey, item.displayName);
            continue;
        }

        container->AddObjectToContainer(base, nullptr, std::max(1, item.count), nullptr);

        // Best-effort: base plus temper plus custom name. The enchantment is the
        // part that genuinely cannot come back.
        Model::ItemExtra extra;
        extra.health = item.reconstruct.health;
        extra.displayName = item.reconstruct.displayName;
        Util::ExtraDataUtil::Apply(container, base, extra);
        ++rebuilt;

        if (!item.reconstruct.playerEnchantment.is_null()) {
            std::string effectSummary;
            if (const auto effects = item.reconstruct.playerEnchantment.find("effects");
                effects != item.reconstruct.playerEnchantment.end() && effects->is_array()) {
                for (const auto& effect : *effects) {
                    if (!effectSummary.empty()) {
                        effectSummary += "; ";
                    }
                    effectSummary +=
                        std::format("{} mag {:.0f} dur {}", effect.value("effectName", "?"),
                                    effect.value("magnitude", 0.0f), effect.value("duration", 0));
                }
            }
            ctx.report.Warn(
                Report::ReasonCode::kPartialByDesign,
                std::format("'{}' was rebuilt from its base record with its temper and name, but "
                            "unenchanted: a player-made enchantment cannot be recreated - there is "
                            "no API to mint one. The original enchantment was: {}",
                            item.displayName, effectSummary.empty() ? "(no effects recorded)"
                                                                    : effectSummary));
        } else {
            ctx.report.Succeeded(subject, itemId, item.reconstruct.baseKey,
                                 std::format("{} (rebuilt)", item.displayName), item.count);
        }
    }

    if (rebuilt > 0) {
        ctx.report.Info(std::format("{} crafted item(s) rebuilt from their base records", rebuilt));
    }
}

}  // namespace SaveMigration::Categories
