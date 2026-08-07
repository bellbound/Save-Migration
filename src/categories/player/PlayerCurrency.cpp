#include "categories/player/PlayerCurrency.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "model/WellKnownForms.h"
#include "util/InventoryCount.h"

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "player.currency";
}

const Core::CategoryDescriptor& PlayerCurrency::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Gold and dragon souls",
        // With the attribute pass: dragon souls are an actor value, and gold has
        // to land before the equipment phase so vendor-value maths is stable.
        .phase = Core::Phase::kEconomy,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void PlayerCurrency::Collect(Core::CollectContext& ctx) {
    auto* player = ctx.player;
    if (!player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);

    auto* gold = Model::WellKnownForms::Get().Gold();
    if (gold) {
        payload["gold"] = Util::CountInInventory(player, gold);
    } else {
        payload["gold"] = nullptr;
        ctx.report.Warn(Report::ReasonCode::kFormLookupFailed,
                        "Gold001 did not resolve, so gold was not recorded");
    }

    if (auto* owner = player->AsActorValueOwner()) {
        payload["dragonSouls"] = owner->GetBaseActorValue(RE::ActorValue::kDragonSouls);
    }

    ctx.report.Succeeded(Report::PlayerSubject(), "currency", "",
                         std::format("{} gold", gold ? Util::CountInInventory(player, gold) : 0));
}

void PlayerCurrency::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::PlayerSubject();
    auto* player = ctx.player;
    if (!player) {
        ctx.report.FailCategory(Report::ReasonCode::kSubjectUnresolvable, "no player");
        return;
    }

    // ── Gold, as a delta ──────────────────────────────────────────────────
    if (const auto goldEntry = payload.find("gold");
        goldEntry != payload.end() && goldEntry->is_number()) {
        auto* gold = Model::WellKnownForms::Get().Gold();
        if (!gold) {
            ctx.report.Failed(subject, "gold", Report::ReasonCode::kFormLookupFailed,
                              "Gold001 did not resolve, so gold could not be restored");
        } else {
            const auto target = goldEntry->get<int32_t>();
            const auto current = Util::CountInInventory(player, gold);
            const auto delta = target - current;
            if (delta > 0) {
                player->AddObjectToContainer(gold, nullptr, delta, nullptr);
            } else if (delta < 0) {
                player->RemoveItem(gold, -delta, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            }
            ctx.report.Succeeded(subject, "gold", "",
                                 std::format("gold {} -> {} (delta {})", current, target, delta));
            ctx.report.Info(
                "gold was applied as a delta rather than an add, so a repeated restore cannot "
                "double it.");
        }
    }

    // ── Dragon souls ──────────────────────────────────────────────────────
    if (const auto souls = payload.find("dragonSouls");
        souls != payload.end() && souls->is_number()) {
        if (auto* owner = player->AsActorValueOwner()) {
            const float value = souls->get<float>();
            if (std::isfinite(value) && value >= 0.0f) {
                owner->SetBaseActorValue(RE::ActorValue::kDragonSouls, value);
                ctx.report.Succeeded(subject, "dragon_souls", "",
                                     std::format("{:.0f} unspent dragon soul(s)", value));
            }
        }
    }
}

}  // namespace SaveMigration::Categories
