#include "categories/npc/NpcSkyrimNetAccompany.h"

#include <format>

#include "defer/PendingWorkQueue.h"
#include "model/FormRef.h"
#include "papyrus/ModProbe.h"
#include "papyrus/PapyrusInterface.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.skyrimnet_accompany";
/// SkyrimNet's Papyrus-facing script.
constexpr std::string_view kSkyrimNetScript = "SkyrimNetApi";
constexpr std::string_view kFollowPackage = "FollowPlayer";

}  // namespace

const Core::CategoryDescriptor& NpcSkyrimNetAccompany::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "SkyrimNet accompany state",
        // Phase 2, and last: registering a package changes AI behaviour and can move
        // the actor out of the loaded set mid-chain.
        .phase = Core::Phase::kIntegrations,
        .restoreMode = Core::RestoreMode::kDeferred,
        .requirement = {.plugins = {},
                        .scriptNames = {},
                        .dllNames = {std::string(Papyrus::Known::kSkyrimNetDll)}},
        .schemaVersion = 1,
    };
    return descriptor;
}

void NpcSkyrimNetAccompany::CollectActor(const Model::ActorSubject& subject,
                                        Core::CollectContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }

    // Projected from observable state, because 'SNPK' is not ours to read.
    bool hasFollowPackage = false;
    if (auto* currentPackage = subject.actor->GetCurrentPackage()) {
        if (const char* editorId = currentPackage->GetFormEditorID();
            editorId && Util::IEquals(editorId, kFollowPackage)) {
            hasFollowPackage = true;
        }
    }

    float waiting = 0.0f;
    if (auto* owner = subject.actor->AsActorValueOwner()) {
        waiting = owner->GetBaseActorValue(RE::ActorValue::kWaitingForPlayer);
    }

    if (!hasFollowPackage && waiting == 0.0f) {
        return;  // neither accompanying nor waiting: nothing to carry
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["hasFollowPackage"] = hasFollowPackage;
    payload["waitingForPlayer"] = waiting;
    if (auto* linked = subject.actor->GetLinkedRef(nullptr)) {
        payload["linkedRef"] = Model::FormKeyUtil::BuildFormKey(linked);
    }

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/skyrimnet_accompany", subject.refKey), subject.refKey,
        std::format("{} ({})", subject.displayName,
                    hasFollowPackage ? "accompanying" : "waiting"));
}

void NpcSkyrimNetAccompany::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (subject.isPlayer || !subject.actor) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object() || !payload.contains("hasFollowPackage")) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};

    Defer::PendingItem item;
    item.categoryId = std::string(kId);
    item.subjectFormKey = subject.refKey;
    item.trigger = Defer::TriggerBits(Defer::Trigger::kActorLoaded);
    item.maxAttempts = 8;
    item.payload = Util::SafeDump(payload);
    if (ctx.pending.Enqueue(std::move(item))) {
        ctx.report.Deferred(subjectRef, std::format("{}/skyrimnet_accompany", subject.refKey),
                            std::format("'{}' accompany state queued until they load - registering a "
                                        "package changes AI and must be the last thing done to them",
                                        subject.displayName));
    }
}

bool NpcSkyrimNetAccompany::ApplyDeferred(const Model::ActorSubject& subject,
                                        Core::ApplyContext& ctx) {
    if (!subject.actor || !subject.actor->Is3DLoaded()) {
        return false;
    }

    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto itemId = std::format("{}/skyrimnet_accompany", subject.refKey);

    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();

    // The waiting AV first: it is plain state and cannot be disturbed by the package.
    if (auto* owner = subject.actor->AsActorValueOwner()) {
        owner->SetBaseActorValue(RE::ActorValue::kWaitingForPlayer,
                                 payload.value("waitingForPlayer", 0.0f));
    }

    if (payload.value("hasFollowPackage", false)) {
        // RegisterPackage(actor, "FollowPlayer", priority, flags, persist).
        const bool dispatched = papyrus->CallGlobalFunction(
            std::string(kSkyrimNetScript), "RegisterPackage",
            {static_cast<RE::Actor*>(subject.actor), std::string(kFollowPackage), 10, 0, true});
        if (!dispatched) {
            ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kPapyrusCallFailed,
                              std::format("SkyrimNet RegisterPackage failed for '{}'",
                                          subject.displayName));
            return true;
        }
    }

    // EvaluatePackage last, so the actor re-derives behaviour from the state we have
    // just finished writing rather than from a half-written intermediate.
    subject.actor->EvaluatePackage();

    ctx.report.Succeeded(subjectRef, itemId, subject.refKey, subject.displayName);
    return true;
}

}  // namespace SaveMigration::Categories
