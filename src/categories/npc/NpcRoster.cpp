#include "categories/npc/NpcRoster.h"

#include <format>
#include <map>

namespace SaveMigration::Categories {

namespace {
constexpr std::string_view kId = "npc.roster";
}

const Core::CategoryDescriptor& NpcRoster::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "NPC roster",
        .phase = Core::Phase::kFingerprint,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void NpcRoster::Collect(Core::CollectContext& ctx) {
    auto& payload = ctx.Payload(kId, Describe().schemaVersion);

    std::map<std::string, uint32_t> byRole;
    if (ctx.subjects) {
        for (const auto& subject : *ctx.subjects) {
            for (const auto& role : subject.roles) {
                ++byRole[role];
            }
        }
    }

    auto roles = nlohmann::json::object();
    for (const auto& [role, count] : byRole) {
        roles[role] = count;
    }
    payload["countsByRole"] = std::move(roles);
    payload["total"] = ctx.subjects ? ctx.subjects->size() : 0;

    ctx.report.Succeeded(Report::SystemSubject("Roster"), "roster", "",
                         std::format("{} actor(s)", ctx.subjects ? ctx.subjects->size() : 0));
    for (const auto& [role, count] : byRole) {
        ctx.report.Info(std::format("roster source '{}' contributed {} actor(s)", role, count));
    }
    if (byRole.empty()) {
        ctx.report.Info(
            "no roster source contributed anything. This is expected on a character with no "
            "followers and no follower-framework mods installed.");
    }
}

void NpcRoster::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::SystemSubject("Roster");

    const size_t expected = payload.value("total", size_t{0});
    size_t resolved = 0;
    size_t unresolved = 0;

    if (ctx.subjects) {
        for (const auto& entry : *ctx.subjects) {
            if (entry.actor) {
                ++resolved;
            } else {
                ++unresolved;
                // Reported individually: "which follower is missing" is exactly the
                // question a user has when a restore looks thin.
                ctx.report.Failed(
                    Report::SubjectRef{Report::SubjectKind::kActor, entry.refKey, entry.displayName},
                    std::format("roster/{}", entry.refKey), Report::ReasonCode::kSubjectUnresolvable,
                    std::format("'{}' could not be found in this save", entry.displayName),
                    entry.refKey, entry.displayName);
            }
        }
    }

    ctx.report.Succeeded(subject, "roster", "",
                         std::format("{}/{} actor(s) resolved", resolved, expected));
    if (unresolved > 0) {
        ctx.report.Warn(Report::ReasonCode::kSubjectUnresolvable,
                        std::format("{} of {} roster actor(s) do not exist in this load order. Their "
                                    "per-actor categories were skipped, not failed.",
                                    unresolved, expected));
    }
}

}  // namespace SaveMigration::Categories
