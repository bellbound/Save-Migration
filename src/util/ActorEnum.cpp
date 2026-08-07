#include "util/ActorEnum.h"

#include <algorithm>
#include <format>
#include <unordered_map>

#include "model/FormRef.h"
#include "model/WellKnownForms.h"
#include "util/StringUtil.h"

namespace SaveMigration::Util {

namespace {

/// A heavy load order can hold well over a million forms. One pass per harvest
/// is acceptable; logging past this many actors is not.
constexpr size_t kActorLogCap = 64;

std::string SafeName(RE::TESObjectREFR* ref) {
    if (!ref) {
        return "";
    }
    const char* name = ref->GetName();
    return (name && *name) ? ConvertSkyrimTextToUTF8(name) : std::format("[{:08X}]", ref->GetFormID());
}

}  // namespace

bool ActorEnum::IsDynamicRef(const RE::TESObjectREFR* ref) {
    return ref && (ref->GetFormID() & 0xFF000000u) == 0xFF000000u;
}

bool ActorEnum::IsReadyForEquip(RE::Actor* actor) {
    // `AddObjectToContainer` works on an unloaded actor; `EquipObject` silently
    // no-ops without 3D. This is the discriminator the deferred queue uses, and
    // the reason inventory is eager while equipment is deferred.
    return actor && actor->Is3DLoaded() && actor->GetCurrent3D() != nullptr;
}

Model::ActorSubject ActorEnum::PlayerSubject() {
    Model::ActorSubject subject;
    subject.isPlayer = true;
    subject.actor = RE::PlayerCharacter::GetSingleton();
    if (subject.actor) {
        subject.base = subject.actor->GetActorBase();
        subject.refKey = Model::FormKeyUtil::BuildFormKey(subject.actor);
        subject.baseKey = Model::FormKeyUtil::BuildFormKey(subject.base);
        subject.displayName = SafeName(subject.actor);
    }
    subject.AddRole("player");
    return subject;
}

void ActorEnum::AddSubject(std::vector<Model::ActorSubject>& out, RE::Actor* actor,
                           std::string_view role) {
    if (!actor) {
        return;
    }
    // The player is handled explicitly and must never be duplicated into the
    // NPC roster - several per-actor categories behave differently for them.
    if (actor->IsPlayerRef()) {
        return;
    }

    const auto refKey = Model::FormKeyUtil::BuildFormKey(actor);
    if (refKey.empty()) {
        // A runtime-spawned actor. Recorded nowhere, because a dynamic FormID is
        // an allocator value private to one save.
        spdlog::debug("ActorEnum: skipping dynamic actor {:08X} for role '{}'", actor->GetFormID(),
                      role);
        return;
    }

    for (auto& existing : out) {
        if (existing.refKey == refKey) {
            existing.AddRole(role);  // union, not replacement
            return;
        }
    }

    Model::ActorSubject subject;
    subject.actor = actor;
    subject.base = actor->GetActorBase();
    subject.refKey = refKey;
    subject.baseKey = Model::FormKeyUtil::BuildFormKey(subject.base);
    subject.displayName = SafeName(actor);
    subject.isDynamicRef = false;
    subject.AddRole(role);
    out.push_back(std::move(subject));
}

void ActorEnum::CollectFactionMembers(std::vector<Model::ActorSubject>& out,
                                      RE::TESFaction* faction, std::string_view role) {
    if (!faction) {
        spdlog::info("ActorEnum: faction for role '{}' unresolved; contributing nothing", role);
        return;
    }

    // Walk the global form map rather than the loaded-actor process lists.
    //
    // This matters: a dismissed follower left in Riften is not in any process
    // list, and there is no base->reference index to find them another way.
    // `IsInFaction` is a virtual that consults both the base record's faction
    // list and the reference's runtime ExtraFactionChanges, so it answers
    // correctly for an actor with no 3D - which is exactly the case we need.
    const auto& [allForms, lock] = RE::TESForm::GetAllForms();
    if (!allForms) {
        spdlog::error("ActorEnum: global form map unavailable");
        return;
    }

    size_t found = 0;
    {
        const RE::BSReadLockGuard guard(lock);
        for (const auto& [formId, form] : *allForms) {
            if (!form) {
                continue;
            }
            auto* actor = form->As<RE::Actor>();
            if (!actor || !actor->IsInFaction(faction)) {
                continue;
            }
            AddSubject(out, actor, role);
            ++found;
        }
    }
    spdlog::info("ActorEnum: role '{}' contributed {} actor(s)", role, found);
}

void ActorEnum::CollectDialogueFollowerAliases(std::vector<Model::ActorSubject>& out) {
    auto* quest = Model::WellKnownForms::Get().DialogueFollowerQuest();
    if (!quest) {
        spdlog::info("ActorEnum: DialogueFollower quest unresolved; contributing nothing");
        return;
    }

    size_t found = 0;
    {
        // The engine mutates this array when aliases fill; take the documented
        // read lock rather than racing it.
        const RE::BSReadLockGuard guard(quest->aliasAccessLock);
        for (auto* alias : quest->aliases) {
            if (!alias) {
                continue;
            }
            auto* refAlias = static_cast<RE::BGSRefAlias*>(alias);
            if (alias->GetVMTypeID() != RE::BGSRefAlias::VMTYPEID) {
                continue;
            }
            if (auto* actor = refAlias->GetActorReference()) {
                AddSubject(out, actor, "dialogue_follower_alias");
                ++found;
            }
        }
    }
    spdlog::info("ActorEnum: DialogueFollower aliases contributed {} actor(s)", found);
}

std::vector<Model::ActorSubject> ActorEnum::BuildForCollect(
    const std::vector<ExtraSource>& extraSources) {
    std::vector<Model::ActorSubject> subjects;

    auto& known = Model::WellKnownForms::Get();

    // ── Vanilla sources ───────────────────────────────────────────────────
    CollectFactionMembers(subjects, known.CurrentFollowerFaction(), "current_follower");
    CollectFactionMembers(subjects, known.DismissedFollowerFaction(), "dismissed_follower");
    CollectDialogueFollowerAliases(subjects);

    // `IsPlayerTeammate` needs no form at all, so it is the one follower signal
    // that cannot be defeated by an unresolved faction. Only loaded actors carry
    // the flag reliably, which is why it supplements rather than replaces the
    // faction sweep.
    if (auto* processLists = RE::ProcessLists::GetSingleton()) {
        size_t found = 0;
        for (auto& handle : processLists->highActorHandles) {
            auto actor = handle.get();
            if (actor && actor->IsPlayerTeammate()) {
                AddSubject(subjects, actor.get(), "current_follower");
                ++found;
            }
        }
        spdlog::info("ActorEnum: IsPlayerTeammate contributed {} loaded actor(s)", found);
    }

    // ── Integration sources ───────────────────────────────────────────────
    // Each arrives as a plain list of reference keys, so ActorEnum needs to know
    // nothing about NFF, MHIYH or SkyrimNet.
    for (const auto& source : extraSources) {
        size_t resolved = 0;
        for (const auto& refKey : source.refKeys) {
            Report::ReasonCode reason = Report::ReasonCode::kNone;
            if (auto* actor = Model::FormResolver::Get().ResolveChecked<RE::Actor>(refKey, reason)) {
                AddSubject(subjects, actor, source.role);
                ++resolved;
            }
        }
        spdlog::info("ActorEnum: role '{}' contributed {}/{} actor(s)", source.role, resolved,
                     source.refKeys.size());
    }

    spdlog::info("ActorEnum: roster is {} actor(s)", subjects.size());
    for (size_t i = 0; i < std::min(subjects.size(), kActorLogCap); ++i) {
        const auto& subject = subjects[i];
        std::string roles;
        for (const auto& role : subject.roles) {
            roles += (roles.empty() ? "" : ",") + role;
        }
        spdlog::debug("  {} [{}] {}", subject.refKey, roles, subject.displayName);
    }
    return subjects;
}

nlohmann::json ActorEnum::RosterToJson(const std::vector<Model::ActorSubject>& subjects) {
    auto actors = nlohmann::json::array();
    for (const auto& subject : subjects) {
        if (subject.isPlayer) {
            continue;  // the player is not part of the NPC roster
        }
        actors.push_back({
            {"refKey", subject.refKey},
            {"baseKey", subject.baseKey},
            {"displayName", subject.displayName},
            {"roles", subject.roles},
        });
    }
    return nlohmann::json{{"count", actors.size()}, {"actors", std::move(actors)}};
}

std::vector<Model::ActorSubject> ActorEnum::BuildForApply(const nlohmann::json& roster) {
    std::vector<Model::ActorSubject> subjects;
    if (!roster.is_object()) {
        return subjects;
    }
    const auto actors = roster.find("actors");
    if (actors == roster.end() || !actors->is_array()) {
        return subjects;
    }

    size_t unresolved = 0;
    for (const auto& entry : *actors) {
        Model::ActorSubject subject;
        subject.refKey = entry.value("refKey", "");
        subject.baseKey = entry.value("baseKey", "");
        subject.displayName = entry.value("displayName", "");
        if (const auto roles = entry.find("roles"); roles != entry.end() && roles->is_array()) {
            for (const auto& role : *roles) {
                if (role.is_string()) {
                    subject.AddRole(role.get<std::string>());
                }
            }
        }

        Report::ReasonCode reason = Report::ReasonCode::kNone;
        subject.actor = Model::FormResolver::Get().ResolveChecked<RE::Actor>(subject.refKey, reason);
        if (subject.actor) {
            subject.base = subject.actor->GetActorBase();
        } else {
            // Kept in the list on purpose. Dropping it here would make the
            // actor vanish from the report as well, and "this follower could not
            // be found" is exactly what the user needs told.
            ++unresolved;
        }
        subjects.push_back(std::move(subject));
    }

    spdlog::info("ActorEnum: roster from snapshot is {} actor(s), {} unresolvable", subjects.size(),
                 unresolved);
    return subjects;
}

}  // namespace SaveMigration::Util
