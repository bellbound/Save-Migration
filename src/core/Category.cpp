#include "core/Category.h"

#include <string>

#include "papyrus/ModProbe.h"

namespace SaveMigration::Core {

namespace {
/// Returned by reference from the const payload accessors when nothing is
/// present, so callers can write `ctx.Payload(id).value(...)` unconditionally.
const nlohmann::json kNullJson{};
}  // namespace

std::string_view ToString(Phase phase) {
    switch (phase) {
        case Phase::kFingerprint:  return "fingerprint";
        case Phase::kIdentity:     return "identity";
        case Phase::kProgression:  return "progression";
        case Phase::kAbilities:    return "abilities";
        case Phase::kEconomy:      return "economy";
        case Phase::kIntegrationsState:      return "integrations_state";
        case Phase::kIntegrationsHomes:      return "integrations_homes";
        case Phase::kIntegrationsOutfits:    return "integrations_outfits";
        case Phase::kIntegrationsAppearance: return "integrations_appearance";
        case Phase::kInventory:    return "inventory";
        case Phase::kEquipment:    return "equipment";
        case Phase::kWorldState:   return "world_state";
        case Phase::kTeleport:     return "teleport";
        case Phase::kFollowers:    return "followers";
        case Phase::kIntegrations: return "integrations";
        case Phase::kSideCar:      return "sidecar";
    }
    return "unknown";
}

nlohmann::json& CollectContext::Payload(std::string_view categoryId, uint32_t schemaVersion) {
    const std::string id(categoryId);
    auto& slot = doc.categories[id];
    if (!slot.is_object()) {
        slot = nlohmann::json::object();
    }
    if (!slot.contains("schemaVersion")) {
        slot["schemaVersion"] = schemaVersion;
        slot["status"] = "ok";
        slot["payload"] = nlohmann::json::object();
    }
    return slot["payload"];
}

nlohmann::json& CollectContext::ActorPayload(std::string_view categoryId,
                                            std::string_view actorKey) {
    const std::string id(categoryId);
    auto& slot = doc.actorCategories[id];
    if (!slot.is_object()) {
        slot = nlohmann::json::object();
    }
    auto& byActor = slot["byActor"];
    if (!byActor.is_object()) {
        byActor = nlohmann::json::object();
    }
    auto& actorSlot = byActor[std::string(actorKey)];
    if (!actorSlot.is_object()) {
        actorSlot = nlohmann::json::object();
    }
    return actorSlot;
}

const nlohmann::json& ApplyContext::Payload(std::string_view categoryId) const {
    const auto it = doc.categories.find(std::string(categoryId));
    if (it == doc.categories.end() || !it->is_object()) {
        return kNullJson;
    }
    const auto payload = it->find("payload");
    return payload == it->end() ? kNullJson : *payload;
}

const nlohmann::json& ApplyContext::ActorPayload(std::string_view categoryId,
                                                 std::string_view actorKey) const {
    const auto categoryIt = doc.actorCategories.find(std::string(categoryId));
    if (categoryIt == doc.actorCategories.end() || !categoryIt->is_object()) {
        return kNullJson;
    }
    const auto byActor = categoryIt->find("byActor");
    if (byActor == categoryIt->end() || !byActor->is_object()) {
        return kNullJson;
    }
    const auto actorIt = byActor->find(std::string(actorKey));
    return actorIt == byActor->end() ? kNullJson : *actorIt;
}

bool ApplyContext::HasPayload(std::string_view categoryId) const {
    return doc.categories.contains(std::string(categoryId)) ||
           doc.actorCategories.contains(std::string(categoryId));
}

uint32_t ApplyContext::PayloadSchemaVersion(std::string_view categoryId) const {
    const auto it = doc.categories.find(std::string(categoryId));
    if (it == doc.categories.end() || !it->is_object()) {
        return 0;
    }
    return it->value("schemaVersion", 0u);
}

bool IGlobalCategory::IsAvailable() const {
    const auto& requirement = Describe().requirement;
    if (requirement.IsVanillaOnly()) {
        return true;
    }
    return Papyrus::ModProbe::Get().IsSatisfied(requirement);
}

bool IActorCategory::IsAvailable() const {
    const auto& requirement = Describe().requirement;
    if (requirement.IsVanillaOnly()) {
        return true;
    }
    return Papyrus::ModProbe::Get().IsSatisfied(requirement);
}

bool IActorCategory::ApplyDeferred(const Model::ActorSubject& subject, ApplyContext& ctx) {
    // Default: the deferred replay is the same work as the instant apply. Only
    // categories with a genuinely different second half (outfits, which must
    // re-arm a lock afterwards) override this.
    ApplyActor(subject, ctx);
    return true;
}

}  // namespace SaveMigration::Core
