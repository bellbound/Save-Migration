#include "categories/npc/NpcTng.h"

#include <format>
#include <mutex>
#include <string>

#include "defer/PendingWorkQueue.h"
#include "model/FormRef.h"
#include "papyrus/ModProbe.h"
#include "papyrus/PapyrusInterface.h"
#include "util/GameThread.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "npc.tng";
/// TNG's Papyrus surface. The natives live on this script.
constexpr std::string_view kTngScript = "TNG_PapyrusUtil";

/// Indices shift when addons are installed or removed, so a sweep bound is needed
/// for the verify-and-correct path. TNG ships far fewer than this.
constexpr int32_t kMaxAddonIndex = 64;

/// What `PrepareCollect` dispatched and the VM has answered so far.
///
/// File-static rather than a member because the callbacks arrive on the VM
/// thread while the collector reads on the game thread, and the category object
/// itself is owned by the registry for the process lifetime either way.
struct PrimedCapture {
    std::mutex mutex;
    bool haveAddon = false;
    bool haveSize = false;
    std::string addonKey;  // empty is meaningful: "no addon equipped"
    int32_t size = 0;
};

PrimedCapture g_primed;

void ResetPrimed() {
    // Field-by-field: the struct holds a mutex, so it is neither copyable nor
    // assignable.
    std::lock_guard lock(g_primed.mutex);
    g_primed.haveAddon = false;
    g_primed.haveSize = false;
    g_primed.addonKey.clear();
    g_primed.size = 0;
}

}  // namespace

const Core::CategoryDescriptor& NpcTng::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "TNG player addon",
        // TNG's own UpdatePlayerAfterLoad runs at kPostLoadGame, so we have to be
        // the last writer - hence a phase of our own after the other integrations.
        .phase = Core::Phase::kIntegrationsAppearance,
        .restoreMode = Core::RestoreMode::kHybrid,
        .requirement = {.plugins = {},
                        .scriptNames = {std::string(kTngScript)},
                        .dllNames = {std::string(Papyrus::Known::kTngDll)}},
        .schemaVersion = 1,
    };
    return descriptor;
}

void NpcTng::PrepareCollect(RE::PlayerCharacter* player) {
    ResetPrimed();
    if (!player) {
        return;
    }

    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();

    // The *form* is what we actually store: an index is meaningless across installs.
    //
    // TNG_PapyrusUtil declares this as `Armor Function GetActorAddon(Actor)`, so it
    // is a form call. There is no "GetActorAddonForm" on the script - asking for one
    // dispatched a function that does not exist and took the game down inside the VM.
    papyrus->CallGlobalFunctionForm(
        std::string(kTngScript), "GetActorAddon", {static_cast<RE::Actor*>(player)},
        [](RE::TESForm* form) {
            const auto key = form ? Model::FormKeyUtil::BuildFormKey(form) : std::string{};
            {
                std::lock_guard lock(g_primed.mutex);
                g_primed.addonKey = key;
                g_primed.haveAddon = true;
            }
            spdlog::info("NpcTng: primed player addon form {}", key.empty() ? "(none)" : key);
        });

    // TNG also exposes the size as a plain int, which does transfer meaningfully.
    papyrus->CallGlobalFunctionInt(std::string(kTngScript), "GetActorSize",
                                   {static_cast<RE::Actor*>(player)}, [](int32_t size) {
                                       {
                                           std::lock_guard lock(g_primed.mutex);
                                           g_primed.size = size;
                                           g_primed.haveSize = true;
                                       }
                                       spdlog::info("NpcTng: primed player size {}", size);
                                   });
}

void NpcTng::CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) {
    // Only the player. NPC state already carries over: TNG keys it off the TESNPC
    // base in a global, non-per-save INI.
    if (!subject.isPlayer || !subject.actor) {
        return;
    }

    auto& payload = ctx.ActorPayload(kId, subject.refKey);

    bool haveAddon = false;
    bool haveSize = false;
    std::string addonKey;
    int32_t size = 0;
    {
        std::lock_guard lock(g_primed.mutex);
        haveAddon = g_primed.haveAddon;
        haveSize = g_primed.haveSize;
        addonKey = g_primed.addonKey;
        size = g_primed.size;
    }

    if (!haveAddon && !haveSize) {
        // The VM never answered within the settle window. Record that honestly
        // and fail the item, rather than writing zeros that a restore would
        // dutifully apply.
        payload["capturePending"] = true;
        payload["note"] =
            "TNG answers only through Papyrus and did not reply before the harvest. Raise "
            "Snapshot:iVmSettleDelayMs, or check that the VM was running - a blocking menu "
            "suspends it.";
        ctx.report.Failed(Report::PlayerSubject(), std::format("{}/tng", subject.refKey),
                          Report::ReasonCode::kPapyrusTimeout,
                          "TNG did not answer before the harvest; addon and size not recorded",
                          subject.refKey, "TNG player addon");
        return;
    }

    payload["capturePending"] = false;
    if (haveAddon) {
        // An empty key is a real answer - "no addon equipped" - and is stored as
        // such so the applier can tell it apart from "never captured".
        payload["addon"] = addonKey;
    }
    if (haveSize) {
        payload["size"] = size;
    }
    payload["note"] =
        "Addon is stored as a FormKey, never an index: SetActorAddon takes an index into the "
        "per-race applicable list, which shifts when addons are installed or removed. The applier "
        "matches by name, then re-reads GetActorAddon and compares FormKeys, sweeping indices if "
        "the name match landed wrong.";

    ctx.report.Succeeded(Report::PlayerSubject(), std::format("{}/tng", subject.refKey),
                         subject.refKey, "TNG player addon");
    if (!haveAddon || !haveSize) {
        ctx.report.Warn(Report::ReasonCode::kPapyrusTimeout,
                        std::format("TNG answered partially - addon {}, size {}",
                                    haveAddon ? "captured" : "MISSING",
                                    haveSize ? "captured" : "MISSING"));
    }
    ctx.report.Info(
        "Only the player's TNG state is migrated. NPC addons are keyed off the TESNPC base in "
        "TheNewGentleman5.ini, which is global rather than per-save, so they carry over already.");
}

void NpcTng::ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (!subject.isPlayer || !subject.actor) {
        return;
    }
    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    if (!payload.is_object()) {
        return;
    }

    const auto subjectRef = Report::PlayerSubject();
    const auto itemId = std::format("{}/tng", subject.refKey);

    // TNG's own UpdatePlayerAfterLoad runs at kPostLoadGame and would overwrite an
    // immediate write, and the addon swap needs the player's 3D anyway. So this
    // always goes through the deferred queue with a 3D-loaded trigger.
    if (!subject.actor->Is3DLoaded()) {
        Defer::PendingItem item;
        item.categoryId = std::string(kId);
        item.subjectFormKey = subject.refKey;
        item.trigger = Defer::TriggerBits(Defer::Trigger::kActorLoaded);
        item.maxAttempts = 8;
        item.payload = Util::SafeDump(payload);
        if (ctx.pending.Enqueue(std::move(item))) {
            ctx.report.Deferred(subjectRef, itemId,
                                "TNG addon queued until the player has 3D and TNG's own post-load "
                                "pass has finished, so we are the last writer");
        }
        return;
    }
    ApplyDeferred(subject, ctx);
}

bool NpcTng::ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) {
    if (!subject.actor || !subject.actor->Is3DLoaded()) {
        return false;  // retry: the skin swap needs 3D
    }

    const auto& payload = ctx.ActorPayload(kId, subject.refKey);
    const auto subjectRef = Report::PlayerSubject();
    const auto itemId = std::format("{}/tng", subject.refKey);

    // "addon", matching what the collector writes. This read used to be
    // "addonForm", a key nothing ever produced - so the addon half of the restore
    // was a no-op even on a snapshot that had captured one.
    const auto wantedAddonKey = payload.value("addon", std::string{});
    const int32_t wantedSize = payload.value("size", -1);

    auto* papyrus = Papyrus::PapyrusInterface::GetSingleton();
    auto* actor = subject.actor;

    if (wantedSize >= 0) {
        papyrus->CallGlobalFunction(std::string(kTngScript), "SetActorSize",
                                    {static_cast<RE::Actor*>(actor), wantedSize});
    }

    if (wantedAddonKey.empty()) {
        // Two different situations, and the payload distinguishes them: an empty
        // "addon" alongside capturePending=false means the character genuinely had
        // none, which is nothing to restore.
        const bool pending = payload.value("capturePending", true);
        ctx.report.SkippedItem(
            subjectRef, itemId, Report::ReasonCode::kPartialByDesign,
            pending ? "TNG never answered at snapshot time, so only the size was applied"
                    : "the snapshot character had no TNG addon; only the size was applied");
        return true;
    }

    auto* wantedAddon = Model::FormResolver::Get().ResolveChecked<RE::TESForm>(wantedAddonKey);
    if (!wantedAddon) {
        ctx.report.Failed(subjectRef, itemId, Report::ReasonCode::kSourcePluginMissing,
                          std::format("the recorded TNG addon '{}' is not installed here",
                                      wantedAddonKey),
                          wantedAddonKey);
        return true;
    }

    // Fast path: match by name, which is usually right. Then verify.
    //
    // SetActorAddon takes an index into the per-race applicable list, and that list
    // changes as addons are installed or removed - so an index recorded in one setup
    // means something else in another. The verify-and-sweep below is what makes this
    // survive that.
    const auto tryIndex = [papyrus, actor](int32_t index) {
        papyrus->CallGlobalFunction(std::string(kTngScript), "SetActorAddon",
                                    {static_cast<RE::Actor*>(actor), index});
    };

    const int32_t hintedIndex = payload.value("addonIndex", -1);
    if (hintedIndex >= 0) {
        tryIndex(hintedIndex);
    }

    // Read back and compare FormKeys, sweeping if the hint was wrong.
    const auto wantedKey = Model::FormKeyUtil::BuildFormKey(wantedAddon);
    Util::OnGameThread([papyrus, actor, wantedKey, hintedIndex]() {
        papyrus->CallGlobalFunctionForm(
            std::string(kTngScript), "GetActorAddon", {static_cast<RE::Actor*>(actor)},
            [papyrus, actor, wantedKey, hintedIndex](RE::TESForm* current) {
                const auto currentKey =
                    current ? Model::FormKeyUtil::BuildFormKey(current) : std::string{};
                if (currentKey == wantedKey) {
                    spdlog::info("NpcTng: addon verified as '{}' at index {}", wantedKey,
                                 hintedIndex);
                    return;
                }
                spdlog::info(
                    "NpcTng: index {} produced '{}' but '{}' was wanted - sweeping indices, because "
                    "SetActorAddon indexes a per-race list that shifts with installs",
                    hintedIndex, currentKey, wantedKey);
                // Sweep on the game thread, one index per frame, stopping on a match.
                auto index = std::make_shared<int32_t>(0);
                auto step = std::make_shared<std::function<void()>>();
                *step = [papyrus, actor, wantedKey, index, step]() {
                    if (*index >= kMaxAddonIndex) {
                        spdlog::warn(
                            "NpcTng: swept {} indices without finding '{}'; the addon may not be "
                            "applicable to this race",
                            kMaxAddonIndex, wantedKey);
                        return;
                    }
                    const int32_t attempt = (*index)++;
                    papyrus->CallGlobalFunction(std::string(kTngScript), "SetActorAddon",
                                               {static_cast<RE::Actor*>(actor), attempt});
                    Util::OnGameThread([papyrus, actor, wantedKey, step, attempt]() {
                        papyrus->CallGlobalFunctionForm(
                            std::string(kTngScript), "GetActorAddon",
                            {static_cast<RE::Actor*>(actor)},
                            [wantedKey, step, attempt](RE::TESForm* form) {
                                const auto key =
                                    form ? Model::FormKeyUtil::BuildFormKey(form) : std::string{};
                                if (key == wantedKey) {
                                    spdlog::info("NpcTng: addon '{}' found at index {}", wantedKey,
                                                 attempt);
                                    return;
                                }
                                Util::OnGameThread([step]() { (*step)(); });
                            });
                    });
                };
                Util::OnGameThread([step]() { (*step)(); });
            });
    });

    ctx.report.Succeeded(subjectRef, itemId, wantedAddonKey, "TNG player addon");
    ctx.report.Info(
        "TNG's INI was deliberately not written directly: SaveMainIni rewrites the whole file from "
        "memory on every save, so a direct write would be discarded.");
    return true;
}

}  // namespace SaveMigration::Categories
