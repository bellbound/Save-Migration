#include "categories/npc/NpcEquipment.h"

#include <algorithm>
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
        // Hybrid, not deferred: every recorded item is attempted and read back at
        // import time, and only what fails to confirm goes on the queue.
        .restoreMode = Core::RestoreMode::kHybrid,
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

    // ── Try first, ask afterwards ─────────────────────────────────────────
    // This used to test `IsReadyForEquip` and queue the whole outfit without
    // attempting anything when the answer was no. That made "an unloaded actor
    // cannot be equipped" an assumption the run never tested - and it is only an
    // assumption: `EquipObject` returns void, so nothing here had ever measured
    // it either way.
    //
    // Now the attempt is made unconditionally and `EquipmentCommon` reads each
    // item back. Whatever confirms is done, for free, however far away the actor
    // is. Only what does not confirm goes on the queue, which is what turns the
    // deferred path into a fallback rather than the default.
    const auto result = EquipmentCommon::Apply(subject.actor, payload["worn"], subjectRef, ctx);

    if (result.unconfirmedKeys.empty()) {
        // Nothing left to wait for. Silent when there was nothing to do at all,
        // because a line saying "0 equipped" for every roster actor who happens to
        // wear nothing recorded is a constant, not a payload.
        if (result.verified > 0 || result.alreadyWorn > 0) {
            ctx.report.Info(std::format("'{}': {} item(s) equipped and confirmed worn during the "
                                        "import{}",
                                        subject.displayName, result.verified,
                                        result.alreadyWorn > 0
                                            ? std::format(", {} already worn", result.alreadyWorn)
                                            : std::string{}));
        }
        return;
    }

    // Queue the remainder only. The payload is rebuilt from the unconfirmed keys
    // rather than embedded whole, so the replay does not walk 32 slots to discover
    // that 30 of them landed an hour ago - and so the queue item shrinks as the
    // outfit lands piece by piece across several attempts.
    auto remainder = nlohmann::json::array();
    for (const auto& entry : payload["worn"]) {
        const auto key = entry.value("form", std::string{});
        if (std::find(result.unconfirmedKeys.begin(), result.unconfirmedKeys.end(), key) !=
            result.unconfirmedKeys.end()) {
            remainder.push_back(entry);
        }
    }

    nlohmann::json queued = nlohmann::json::object();
    queued["worn"] = std::move(remainder);
    if (const auto cell = payload.find("cell"); cell != payload.end()) {
        queued["cell"] = *cell;
    }

    Defer::PendingItem item;
    item.categoryId = std::string(kId);
    item.subjectFormKey = subject.refKey;
    item.cellFormKey = payload.value("cell", std::string{});
    item.trigger = Defer::TriggerBits(Defer::Trigger::kActorLoaded) |
                   Defer::TriggerBits(Defer::Trigger::kCellFullyLoaded);
    item.maxAttempts = static_cast<uint32_t>(Config::MigrationConfig::DeferMaxAttempts());
    item.payload = Util::SafeDump(queued);

    if (ctx.pending.Enqueue(std::move(item))) {
        // No `Deferred` line here, on purpose. The import's settle pass runs after
        // the follower regroup has brought these actors to the player, and it
        // drains most of this queue before the run ends - so a line claiming the
        // gear is waiting would be written before we know whether it is. The
        // orchestrator reports whatever genuinely survives the settle.
        spdlog::debug("NpcEquipment: {} of {} item(s) for '{}' did not confirm; queued",
                      result.unconfirmed, result.unconfirmed + result.verified + result.alreadyWorn,
                      subject.displayName);
    } else {
        ctx.report.Failed(subjectRef, std::format("{}/equipment", subject.refKey),
                          Report::ReasonCode::kDeferredExhausted,
                          std::format("could not queue the remaining {} item(s) of equipment for "
                                      "'{}' (queue full or payload oversized)",
                                      result.unconfirmed, subject.displayName));
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
    ctx.report.Info(std::format("deferred equipment for '{}': {} confirmed worn, {} did not "
                                "confirm, {} could not be attempted",
                                subject.displayName, result.verified, result.unconfirmed,
                                result.failed));

    // The actor was loaded, the engine took the equip, and the item still does not
    // read back as worn. Retrying is what the attempt counter is for and it has
    // been spent, so this is where it stops being "not yet" and becomes an outcome.
    for (const auto& key : result.unconfirmedKeys) {
        ctx.report.Failed(subjectRef, std::format("{}/equip/{}", subject.refKey, key),
                          Report::ReasonCode::kValidationMismatch,
                          std::format("'{}' was loaded and the equip was accepted, but the item "
                                      "did not read back as worn",
                                      subject.displayName),
                          key);
    }
    return true;
}

}  // namespace SaveMigration::Categories
