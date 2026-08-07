#include "categories/npc/NpcInventory.h"

#include <format>

#include "config/MigrationConfig.h"
#include "model/FormRef.h"

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "npc.inventory";
}

const Core::CategoryDescriptor& NpcInventory::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "NPC inventories",
        .phase = Core::Phase::kFollowers,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void NpcInventory::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }

    auto collected = InventoryCommon::Collect(subject.actor);

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    const auto itemCount = collected.items.size();
    payload["items"] = std::move(collected.items);
    payload["unmigratable"] = std::move(collected.unmigratable);

    // The default and sleep outfits are recorded because a fresh save's NPC
    // re-equips their default outfit when their 3D loads, and that re-equip will
    // fight ours. Knowing what it is lets the equipment pass reason about it.
    if (subject.base) {
        payload["defaultOutfit"] = Model::FormKeyUtil::BuildFormKey(subject.base->defaultOutfit);
        payload["sleepOutfit"] = Model::FormKeyUtil::BuildFormKey(subject.base->sleepOutfit);
    }

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/inventory", subject.refKey), subject.refKey,
        std::format("{} ({} entries)", subject.displayName, itemCount));
}

void NpcInventory::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    const auto items = payload.find("items");
    if (items == payload.end() || !items->is_array()) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};

    auto& cursor = m_cursors[subject.refKey];
    if (cursor.finished) {
        return;
    }

    // Budget shared across all actors in this frame, so twenty followers do not
    // each get a full per-frame allowance.
    const auto perFrame = static_cast<uint32_t>(
        std::max(1, Config::MigrationConfig::ItemsPerFrame() /
                        static_cast<int>(std::max<size_t>(ctx.subjects ? ctx.subjects->size() : 1, 1))));

    if (InventoryCommon::ApplyChunk(subject.actor, *items, subjectRef, ctx, cursor, perFrame)) {
        ctx.RequestContinuation();
        return;
    }

    if (const auto unmigratable = payload.find("unmigratable"); unmigratable != payload.end()) {
        InventoryCommon::ApplyUnmigratable(subject.actor, *unmigratable, subjectRef, ctx);
    }
}

}  // namespace SaveMigration::Categories
