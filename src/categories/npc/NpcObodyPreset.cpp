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

void NpcObodyPreset::ResetPrimed() {
    m_noPresetCount = 0;
    std::lock_guard lock(m_primed.mutex);
    m_primed.presets.clear();
    m_primed.distributionKey.clear();
    m_primed.haveDistributionKey = false;
}

void NpcObodyPreset::PrepareCollect(RE::PlayerCharacter* player,
                                    const std::vector<Model::ActorSubject>& roster) {
    ResetPrimed();

    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    if (!papyrus) {
        return;
    }

    // The distribution key first: it is what stops OBody re-randomising everyone
    // it has already seen, so its value matters more than any single preset.
    papyrus->CallGlobalFunctionString(
        std::string(kStorageUtil), "GetStringValue",
        {std::monostate{}, std::string(kDistributionKey), std::string{}},
        [this](const std::string& value) {
            {
                std::lock_guard lock(m_primed.mutex);
                m_primed.distributionKey = value;
                m_primed.haveDistributionKey = true;
            }
            spdlog::info("NpcObodyPreset: primed distribution key '{}'", value);
        });

    // One read per actor, dispatched now so the answers land during the settle
    // delay. The player is dispatched explicitly rather than relying on the
    // roster, which by construction never contains them.
    const auto prime = [this, papyrus](RE::Actor* actor, std::string_view who) {
        if (!actor) {
            return;
        }
        const auto formId = actor->GetFormID();
        const auto key = PresetKeyFor(formId);
        papyrus->CallGlobalFunctionString(
            std::string(kStorageUtil), "GetStringValue",
            {static_cast<RE::TESForm*>(actor), key, std::string{}},
            [this, formId, label = std::string(who)](const std::string& preset) {
                {
                    std::lock_guard lock(m_primed.mutex);
                    m_primed.presets[formId] = preset;
                }
                if (!preset.empty()) {
                    spdlog::info("NpcObodyPreset: primed {} -> preset '{}'", label, preset);
                }
            });
    };

    prime(player, "player");
    uint32_t dispatched = 0;
    for (const auto& subject : roster) {
        if (subject.isPlayer || !subject.actor) {
            continue;
        }
        prime(subject.actor, subject.displayName);
        ++dispatched;
    }
    spdlog::info("NpcObodyPreset: primed the player and {} roster actor(s)", dispatched);
}

void NpcObodyPreset::BeginCollect(Core::CollectContext& ctx) {
    // No `distributionKeyName`. It was `kDistributionKey` written out verbatim -
    // a constant this build already knows, recorded as though it were something
    // read off the save.
    auto& payload = ctx.Payload(kId, Describe().schemaVersion);

    std::lock_guard lock(m_primed.mutex);
    if (m_primed.haveDistributionKey) {
        // Recorded, not restored. What OBody does when this value differs from
        // the one it finds is its own business, and writing another mod's global
        // state on an inference is how a redistribution gets triggered - the exact
        // thing this category exists to avoid. Carrying it means a future build
        // can act on it without needing another export.
        payload["distributionKey"] = m_primed.distributionKey;
    } else {
        ctx.report.Warn(Report::ReasonCode::kPapyrusTimeout,
                        "OBody's distribution key was not read back before the harvest; it is "
                        "recorded as absent rather than as empty");
    }
}

void NpcObodyPreset::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    if (!subject.actor) {
        return;
    }

    const auto key = PresetKeyFor(subject.actor->GetFormID());
    const Report::SubjectRef subjectRef{Report::SubjectKind::kActor, subject.refKey,
                                        subject.displayName};
    const auto itemId = std::format("{}/obody", subject.refKey);

    // Only what `PrepareCollect` actually got an answer for. Dispatching the read
    // here instead would guarantee it arrives after the document is written,
    // which is precisely how this category came to record nothing at all.
    std::string preset;
    bool answered = false;
    {
        std::lock_guard lock(m_primed.mutex);
        if (const auto it = m_primed.presets.find(subject.actor->GetFormID());
            it != m_primed.presets.end()) {
            preset = it->second;
            answered = true;
        }
    }

    if (!answered) {
        // An actor the prime roster did not hold, or a VM that did not reply in
        // time. Either way it is "not captured", which is a different fact from
        // "captured, and there is no preset" - and the applier must not treat a
        // silence as an instruction to do nothing.
        auto& payload = ctx.ActorPayload(kId, subject.refKey);
        payload["storageKey"] = key;
        payload["capturePending"] = true;
        ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kPapyrusTimeout,
                          std::format("OBody did not answer for '{}' before the harvest; no preset "
                                      "recorded",
                                      subject.displayName),
                          subject.refKey);
        return;
    }

    if (preset.empty()) {
        // Answered, and the answer is that OBody has nothing stored for this
        // actor. Nothing is written and nothing is reported per actor: it used to
        // record `{storageKey, capturePending: false, preset: ""}` and file a
        // `partial_by_design` skip for every one, so a roster of twenty-five NPCs
        // that OBody had simply never touched produced twenty-five payload entries
        // holding nothing and twenty-five identical lines in the report. An absent
        // entry says the same thing, and the applier reads it the same way. The
        // count is summarised once in `EndCollect`.
        ++m_noPresetCount;
        return;
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);
    payload["storageKey"] = key;
    payload["capturePending"] = false;
    payload["preset"] = preset;

    ctx.report.Succeeded(subjectRef, itemId, subject.refKey,
                         std::format("{} ({})", subject.displayName, preset));
}

void NpcObodyPreset::EndCollect(Core::CollectContext& ctx) {
    if (m_noPresetCount > 0) {
        ctx.report.Info(std::format(
            "{} roster actor(s) have no OBody preset stored and were not recorded. OBody assigns a "
            "preset the first time it sees a body, so an actor it has never rendered has nothing "
            "to carry.",
            m_noPresetCount));
    }
    m_noPresetCount = 0;
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
