#include "categories/npc/NpcOutfitVrDressUp.h"

#include <Windows.h>

#include <format>
#include <vector>

#include "defer/PendingWorkQueue.h"
#include "papyrus/DressUpInterface002.h"
#include "papyrus/ModProbe.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.outfit_vr_dressup";

/// VR Dress Up's own name for the outfit it enforces.
constexpr const char* kLockedOutfit = "locked";

DressUp::Interface002* GetDressUpApi() {
    using GetInterfaceFn = void* (*)(unsigned int);
    static DressUp::Interface002* cached = nullptr;
    static bool attempted = false;
    if (attempted) {
        return cached;
    }
    attempted = true;

    HMODULE module = GetModuleHandleA("DressUpVR.dll");
    if (!module) {
        return nullptr;
    }
    auto fn = reinterpret_cast<GetInterfaceFn>(GetProcAddress(module, "GetDressUpInterface"));
    if (!fn) {
        spdlog::warn("NpcOutfitVrDressUp: DressUpVR.dll has no GetDressUpInterface export");
        return nullptr;
    }
    cached = static_cast<DressUp::Interface002*>(fn(2));
    if (!cached) {
        spdlog::warn(
            "NpcOutfitVrDressUp: DressUpVR.dll is present but does not provide interface v2. "
            "Outfits cannot be enumerated or injected without it - update VR Dress Up.");
        return nullptr;
    }
    spdlog::info("NpcOutfitVrDressUp: DressUp interface v{} build {}", cached->GetVersion(),
                 cached->GetBuild());
    return cached;
}

std::vector<std::string> ToVector(const DressUp::StringList& list) {
    std::vector<std::string> result;
    result.reserve(list.count);
    for (uint32_t i = 0; i < list.count; ++i) {
        if (list.items[i]) {
            result.emplace_back(list.items[i]);
        }
    }
    return result;
}

std::vector<const char*> ToPointers(const std::vector<std::string>& values) {
    std::vector<const char*> pointers;
    pointers.reserve(values.size());
    for (const auto& value : values) {
        pointers.push_back(value.c_str());
    }
    return pointers;
}

}  // namespace

const Core::CategoryDescriptor& NpcOutfitVrDressUp::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "VR Dress Up outfits",
        .phase = Core::Phase::kIntegrationsOutfits,
        .restoreMode = Core::RestoreMode::kHybrid,
        .requirement = {.plugins = {},
                        .scriptNames = {},
                        .dllNames = {std::string(Papyrus::Known::kDressUpDll)}},
        .schemaVersion = 1,
    };
    return descriptor;
}

bool NpcOutfitVrDressUp::IsAvailable() const {
    // The DLL being loaded is not enough: it also has to expose interface v2. An
    // older VR Dress Up reports "installed" and then cannot be driven at all.
    return GetDressUpApi() != nullptr;
}

void NpcOutfitVrDressUp::CollectActor(const Model::ActorSubject& subject,
                                     Core::CollectContext& ctx) {
    auto* api = GetDressUpApi();
    if (!api || subject.isPlayer || !subject.actor) {
        return;
    }

    const auto outfitNames = ToVector(api->EnumerateOutfits(subject.actor));
    if (outfitNames.empty()) {
        return;
    }

    auto outfits = nlohmann::json::object();
    for (const auto& name : outfitNames) {
        // Copy immediately: the returned list is borrowed and the next call
        // invalidates it.
        const auto items = ToVector(api->EnumerateOutfitItems(subject.actor, name.c_str()));
        if (!items.empty()) {
            outfits[name] = items;
        }
    }
    if (outfits.empty()) {
        return;
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["outfits"] = std::move(outfits);
    payload["playerGiven"] = ToVector(api->EnumeratePlayerGivenItems(subject.actor));
    payload["wasLocked"] = api->IsActorLocked(subject.actor);

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/vr_dressup", subject.refKey), subject.refKey,
        std::format("{} ({} outfit(s))", subject.displayName, payload["outfits"].size()));
}

void NpcOutfitVrDressUp::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    auto* api = GetDressUpApi();
    if (!api || subject.isPlayer || !subject.actor) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    const auto outfits = payload.find("outfits");
    if (outfits == payload.end() || !outfits->is_object()) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto itemId = std::format("{}/vr_dressup", subject.refKey);

    // ── Step 1: map injection, and nothing else ───────────────────────────
    // SetOutfitByFormKeys writes storage only. Deliberately no equip here: an equip
    // now would run ApplyOutfit against an inventory the inventory phase has not
    // filled yet, and the prune inside it would delete the outfit we just injected.
    uint32_t injected = 0;
    for (auto it = outfits->begin(); it != outfits->end(); ++it) {
        if (!it.value().is_array()) {
            continue;
        }
        std::vector<std::string> keys;
        for (const auto& key : it.value()) {
            if (key.is_string()) {
                keys.push_back(key.get<std::string>());
            }
        }
        const auto pointers = ToPointers(keys);
        injected += api->SetOutfitByFormKeys(subject.actor, it.key().c_str(), pointers.data(),
                                            static_cast<uint32_t>(pointers.size()));
    }

    if (const auto playerGiven = payload.find("playerGiven");
        playerGiven != payload.end() && playerGiven->is_array()) {
        std::vector<std::string> keys;
        for (const auto& key : *playerGiven) {
            if (key.is_string()) {
                keys.push_back(key.get<std::string>());
            }
        }
        const auto pointers = ToPointers(keys);
        api->MarkPlayerGivenByFormKeys(subject.actor, pointers.data(),
                                       static_cast<uint32_t>(pointers.size()));
    }

    ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                         std::format("{} ({} item(s) injected into storage)", subject.displayName,
                                     injected));

    // ── Steps 3-5 are deferred: they need 3D ──────────────────────────────
    Defer::PendingItem item;
    item.categoryId = std::string(kId);
    item.subjectFormKey = subject.refKey;
    item.trigger = Defer::TriggerBits(Defer::Trigger::kActorLoaded) |
                   Defer::TriggerBits(Defer::Trigger::kCellFullyLoaded);
    item.maxAttempts = 8;
    item.payload = Util::SafeDump(payload);
    if (ctx.pending.Enqueue(std::move(item))) {
        ctx.report.Deferred(subjectRef, std::format("{}/vr_dressup_equip", subject.refKey),
                            std::format("'{}' outfit stored now; the equip and the lock re-arm wait "
                                        "until they load, so ApplyOutfit cannot prune against an "
                                        "unfilled inventory",
                                        subject.displayName));
    }
}

bool NpcOutfitVrDressUp::ApplyDeferred(const Model::ActorSubject& subject,
                                      Core::ApplyContext& ctx) {
    auto* api = GetDressUpApi();
    if (!api) {
        return true;
    }
    if (!subject.actor || !subject.actor->Is3DLoaded()) {
        return false;  // equipping without 3D silently does nothing
    }

    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    const auto outfits = payload.find("outfits");
    if (outfits == payload.end() || !outfits->is_object()) {
        return true;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto itemId = std::format("{}/vr_dressup_equip", subject.refKey);

    // ── Step 3: close the inventory gap before anything can prune ─────────
    uint32_t topped = 0;
    for (auto it = outfits->begin(); it != outfits->end(); ++it) {
        topped += api->EnsureOutfitItemsInInventory(subject.actor, it.key().c_str());
    }

    // ── Step 4: now the equip is safe ─────────────────────────────────────
    const bool hasLocked = outfits->contains(kLockedOutfit);
    bool applied = false;
    if (hasLocked) {
        applied = api->ApplyOutfitNow(subject.actor, kLockedOutfit, /*unequipOthers=*/true);
    } else {
        // No "locked" outfit recorded: apply the first stored one so the NPC at least
        // looks right, without asserting a lock they never had.
        const auto first = outfits->begin();
        applied = api->ApplyOutfitNow(subject.actor, first.key().c_str(), /*unequipOthers=*/true);
    }

    // ── Step 5: re-arm the lock last ──────────────────────────────────────
    // Lock re-derives "locked" from what is *currently worn*, so doing this before
    // step 4 would freeze whatever the NPC happened to have on.
    if (payload.value("wasLocked", false)) {
        if (!api->IsActorLocked(subject.actor)) {
            api->LockActor(subject.actor);
        }
    }

    if (applied) {
        ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                             std::format("{} ({} item(s) topped up, outfit applied{})",
                                         subject.displayName, topped,
                                         payload.value("wasLocked", false) ? ", lock re-armed" : ""));
    } else {
        ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kOutfitItemSkipped,
                          std::format("VR Dress Up refused to apply '{}' outfit for '{}'",
                                      hasLocked ? kLockedOutfit : "first stored",
                                      subject.displayName));
    }
    return true;
}

}  // namespace SaveMigration::Categories
