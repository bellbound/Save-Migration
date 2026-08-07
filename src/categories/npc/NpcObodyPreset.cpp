#include "categories/npc/NpcObodyPreset.h"

#include <format>

#include "defer/PendingWorkQueue.h"
#include "papyrus/ModProbe.h"
#include "papyrus/PapyrusInterface.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.obody_preset";
/// StorageUtil is PapyrusUtil's; OBody stores through it rather than in its own
/// co-save, which is why this is reachable at all.
constexpr std::string_view kStorageUtil = "StorageUtil";
/// The key OBody uses to remember which actors it has already distributed to.
constexpr std::string_view kDistributionKey = "obody_ng_distribution_key";

}  // namespace

std::string NpcObodyPreset::PresetKeyFor(RE::FormID formId) {
    // Papyrus stringifies an Int in *signed* decimal. The player is 0x14 == 20, so
    // the key is "obody_20_preset". The signed cast is load-bearing: a form ID above
    // 0x7FFFFFFF stringifies negative on the Papyrus side, and an unsigned format
    // here would address a key that does not exist.
    return std::format("obody_{}_preset", static_cast<int32_t>(formId));
}

const Core::CategoryDescriptor& NpcObodyPreset::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "OBody presets",
        // Before any actor generates a body: OBody assigns a random preset the first
        // time it sees one, and once assigned the distribution key makes it sticky.
        .phase = Core::Phase::kIntegrationsState,
        .restoreMode = Core::RestoreMode::kHybrid,
        .requirement = {.plugins = {},
                        .scriptNames = {std::string(kStorageUtil)},
                        .dllNames = {std::string(Papyrus::Known::kObodyDll)}},
        .schemaVersion = 1,
    };
    return descriptor;
}

void NpcObodyPreset::BeginCollect(Core::CollectContext& ctx) {
    // The distribution key first: it is what stops OBody re-randomising everyone,
    // so it matters more than any individual preset.
    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    auto& payload = ctx.Payload(kId, Describe().schemaVersion);

    papyrus->CallGlobalFunctionString(
        std::string(kStorageUtil), "GetStringValue",
        {std::monostate{}, std::string(kDistributionKey), std::string{}},
        [](const std::string& value) {
            spdlog::info("NpcObodyPreset: distribution key '{}'", value);
        });
    payload["distributionKeyName"] = std::string(kDistributionKey);
}

void NpcObodyPreset::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (!subject.actor) {
        return;
    }

    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    const auto key = PresetKeyFor(subject.actor->GetFormID());
    const auto refKey = subject.refKey;

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["storageKey"] = key;

    papyrus->CallGlobalFunctionString(
        std::string(kStorageUtil), "GetStringValue",
        {static_cast<RE::TESForm*>(subject.actor), key, std::string{}},
        [refKey](const std::string& preset) {
            if (!preset.empty()) {
                spdlog::info("NpcObodyPreset: {} -> preset '{}'", refKey, preset);
            }
        });

    ctx.report.Succeeded(
        Report::SubjectRef{Report::SubjectKind::kActor, subject.refKey, subject.displayName},
        std::format("{}/obody", subject.refKey), subject.refKey, subject.displayName);
}

void NpcObodyPreset::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (!subject.actor) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    const auto preset = payload.value("preset", std::string{});
    if (preset.empty()) {
        return;
    }

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};

    // The *key* is written now, before anything generates a body - that is the whole
    // point of this phase. The visible morph application is deferred to after the
    // final outfit and skin, because ORefit computes clothing morphs from the worn
    // slot-32 armour.
    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    const auto key = PresetKeyFor(subject.actor->GetFormID());
    papyrus->CallGlobalFunction(std::string(kStorageUtil), "SetStringValue",
                                {static_cast<RE::TESForm*>(subject.actor), key, preset});

    Defer::PendingItem item;
    item.categoryId = std::string(kId);
    item.subjectFormKey = subject.refKey;
    item.trigger = Defer::TriggerBits(Defer::Trigger::kActorLoaded);
    item.maxAttempts = 8;
    item.payload = Util::SafeDump(payload);
    if (ctx.pending.Enqueue(std::move(item))) {
        ctx.report.Deferred(subjectRef, std::format("{}/obody", subject.refKey),
                            std::format("preset key for '{}' written now; the morph application "
                                        "waits until the final outfit and skin are on, because "
                                        "ORefit derives clothing morphs from worn slot 32",
                                        subject.displayName));
    }
}

bool NpcObodyPreset::ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (!subject.actor || !subject.actor->Is3DLoaded()) {
        return false;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    const auto preset = payload.value("preset", std::string{});
    if (preset.empty()) {
        return true;
    }

    // ApplyPresetByName, and deliberately *not* MarkForReprocess: reprocessing
    // invites the redistribution the distribution key exists to prevent.
    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    const bool dispatched =
        papyrus->CallGlobalFunction("OBodyNative", "ApplyPresetByName",
                                    {static_cast<RE::Actor*>(subject.actor), preset});

    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    if (dispatched) {
        ctx.report.Succeeded(subjectRef, std::format("{}/obody", subject.refKey), "", preset);
    } else {
        ctx.report.Failed(subjectRef, std::format("{}/obody", subject.refKey),
                          Report::ReasonCode::kPapyrusCallFailed,
                          std::format("OBodyNative.ApplyPresetByName('{}') could not be dispatched",
                                      preset));
    }
    return true;
}

}  // namespace SaveMigration::Categories
