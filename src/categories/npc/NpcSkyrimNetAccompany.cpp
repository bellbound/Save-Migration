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
/// The name SkyrimNet's *API* takes. It is a friendly name in a lookup table, not
/// the name of anything in the game.
constexpr std::string_view kFollowPackage = "FollowPlayer";
/// The name the *form* actually has. `PackageFormCache` maps "FollowPlayer" onto
/// this editor ID, so comparing a running package against the friendly name - which
/// is what this category did - could never match, and `hasFollowPackage` was false
/// in every snapshot ever taken. Every entry that did get recorded got in through
/// the waiting actor value instead.
constexpr std::string_view kFollowPackageEditorId = "SkyrimNet_PlayerFollowPackage";

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

    // Projected from observable state, because 'SNPK' is not ours to read. This is
    // the cheap synchronous signal and it only sees the package the actor is running
    // *right now* - an accompanying NPC mid-sandbox is not running it. The
    // authoritative answer comes from `SkyrimNetApi.HasPackage`, which
    // `FollowerRegroup` primes; this stays as the answer for when the VM does not
    // reply in time.
    bool hasFollowPackage = false;
    if (auto* currentPackage = subject.actor->GetCurrentPackage()) {
        if (const char* editorId = currentPackage->GetFormEditorID();
            editorId && Util::IEquals(editorId, kFollowPackageEditorId)) {
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
    const auto& recorded = ctx.ActorPayload(kId, subject.refKey);
    const bool haveRecorded = recorded.is_object() && recorded.contains("hasFollowPackage");

    // Anyone who was actively following at export gets the package, whatever was
    // making them follow over there. Most followers are recruited through vanilla
    // dialogue and carry no SkyrimNet package at all, so `CollectActor` records
    // nothing for them - and the restore used to leave them standing next to the
    // player, moved but not following.
    //
    // `npc.follower_regroup` is asked rather than the roster role, because it is the
    // one place that decides who was actually following: the `current_follower` role
    // is faction membership, which mod followers keep after dismissal. Payload reads
    // do not depend on apply order - the whole document is present before phase one.
    //
    // The package rather than `SetPlayerTeammate`: the vanilla teammate flag drags
    // the whole DialogueFollower quest behind it and a follower resuming through it
    // mid-restore behaves badly, which is what the old note was really about.
    const auto& regroup = ctx.ActorPayload("npc.follower_regroup", subject.refKey);
    const bool wasFollower = regroup.is_object() && regroup.value("wasActiveFollower", false);

    if (!haveRecorded && !wasFollower) {
        return;
    }

    // A dead follower is not a follower. Registering a package on a corpse is
    // wasted queue budget at best, and `npc.life_state` has already put them back
    // the way they were by the time this runs.
    if (subject.actor->IsDead()) {
        return;
    }

    nlohmann::json payload =
        haveRecorded ? recorded : nlohmann::json::object({{"waitingForPlayer", 0.0f}});

    // Told to wait, and recorded as such, stays told to wait: that is a deliberate
    // instruction the player gave this character, and it is exactly as much "was
    // following" as accompanying is. Only the ones we have no wait on are switched
    // to accompanying.
    const bool wasWaiting = payload.value("waitingForPlayer", 0.0f) != 0.0f;
    if (wasFollower && !wasWaiting && !payload.value("hasFollowPackage", false)) {
        payload["hasFollowPackage"] = true;
        payload["followPackageFromRole"] = true;
    }

    const bool fromRole = payload.value("followPackageFromRole", false);

    Defer::PendingItem item;
    item.categoryId = std::string(kId);
    item.subjectFormKey = subject.refKey;
    item.trigger = Defer::TriggerBits(Defer::Trigger::kActorLoaded);
    item.maxAttempts = 8;
    item.payload = Util::SafeDump(payload);
    if (ctx.pending.Enqueue(std::move(item))) {
        // Not reported here. This category sits at phase 90, four phases before the
        // regroup teleports these actors to the player, so at this moment every one
        // of them is unloaded by construction - and the settle pass then registers
        // the package for the whole party before the run ends. A `Deferred` line
        // written now would be wrong for almost everybody it named.
        spdlog::debug("NpcSkyrimNetAccompany: '{}' queued ({})", subject.displayName,
                      fromRole ? "was following at export" : "recorded accompany state");
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

    ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                         std::format("{} ({})", subject.displayName,
                                     payload.value("hasFollowPackage", false) ? "accompanying"
                                                                              : "waiting"));
    return true;
}

}  // namespace SaveMigration::Categories
