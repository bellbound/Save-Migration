#include "categories/npc/NpcEquipment.h"

#include <format>

#include "categories/EquipmentCommon.h"
#include "config/MigrationConfig.h"
#include "defer/PendingWorkQueue.h"
#include "model/FormRef.h"
#include "util/ActorEnum.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "npc.equipment";
}

const Core::CategoryDescriptor& NpcEquipment::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "NPC equipment",
        .phase = Core::Phase::kFollowers,
        .restoreMode = Core::RestoreMode::kDeferred,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void NpcEquipment::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }

    auto worn = EquipmentCommon::Collect(subject.actor);
    if (worn.empty()) {
        return;
    }
    const auto count = worn.size();

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["worn"] = std::move(worn);
    // The cell is recorded so the deferred queue can also fire on "the player
    // entered this NPC's cell", not only on "this NPC loaded".
    if (auto* cell = subject.actor->GetParentCell()) {
        payload["cell"] = Model::FormKeyUtil::BuildFormKey(cell);
    }

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/equipment", subject.refKey), subject.refKey,
        std::format("{} ({} worn)", subject.displayName, count));
}

void NpcEquipment::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (subject.isPlayer) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object() || !payload.contains("worn")) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};

    // If the actor happens to be loaded right now, do it immediately rather than
    // making the user walk back to them.
    if (Util::ActorEnum::IsReadyForEquip(subject.actor)) {
        const auto result = EquipmentCommon::Apply(subject.actor, payload["worn"], subjectRef, ctx);
        ctx.report.Info(std::format("'{}' was already loaded: {} equipped immediately",
                                    subject.displayName, result.equipped));
        return;
    }

    // Otherwise queue it. The payload is embedded whole, so the queue survives the
    // snapshot directory being deleted or the mod folder moving.
    Defer::PendingItem item;
    item.categoryId = std::string(kId);
    item.subjectFormKey = subject.refKey;
    item.cellFormKey = payload.value("cell", std::string{});
    item.trigger = Defer::TriggerBits(Defer::Trigger::kActorLoaded) |
                   Defer::TriggerBits(Defer::Trigger::kCellFullyLoaded);
    item.maxAttempts = static_cast<uint32_t>(Config::MigrationConfig::DeferMaxAttempts());
    item.payload = Util::SafeDump(payload);

    if (ctx.pending.Enqueue(std::move(item))) {
        ctx.report.Deferred(subjectRef, std::format("{}/equipment", subject.refKey),
                            std::format("'{}' is not loaded; their gear will be equipped when you "
                                        "next see them",
                                        subject.displayName));
    } else {
        ctx.report.Failed(subjectRef, std::format("{}/equipment", subject.refKey),
                          Report::ReasonCode::kDeferredExhausted,
                          std::format("could not queue equipment for '{}' (queue full or payload "
                                      "oversized)",
                                      subject.displayName));
    }
}

bool NpcEquipment::ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object() || !payload.contains("worn")) {
        return true;  // nothing to do: retire rather than retry forever
    }

    if (!Util::ActorEnum::IsReadyForEquip(subject.actor)) {
        return false;  // not ready: the manager will retry on the next trigger
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto result = EquipmentCommon::Apply(subject.actor, payload["worn"], subjectRef, ctx);

    // A partial result still retires: the failures are individually reported, and
    // retrying would re-report them every time the actor loads.
    ctx.report.Info(std::format("deferred equipment for '{}': {} equipped, {} failed",
                                subject.displayName, result.equipped, result.failed));
    return true;
}

}  // namespace SaveMigration::Categories
