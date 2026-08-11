#include "categories/npc/NpcTng.h"

#include <charconv>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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

// ═══════════════════════════════════════════════════════════════════════════
// Reading TNG without asking TNG.
//
// The Papyrus route above is TNG's public surface, and it is honest, but it is
// also asynchronous: the harvest is one game-thread task, so a call dispatched
// inside `Collect` cannot answer before `Collect` ends. That is what
// `PrepareCollect` and iVmSettleDelayMs exist to work around, and it is a race
// that the plugin loses whenever the VM is busy or suspended - a snapshot taken
// on 2026-08-08 recorded `capturePending: true` and nothing else, which is
// exactly the failure it is meant to prevent.
//
// It does not have to be a race. TNG keeps both values in ordinary game data
// that any SKSE plugin can read on the spot:
//
//   - the addon index is a *keyword* on the actor's TESNPC, named
//     "TNG_ActorAddnAuto:NN" or "TNG_ActorAddnUser:NN" (Core::OrganizeNPCKeywords),
//     where NN indexes TNG's master addon list, and
//   - the size category is one of five keywords, TheNewGentleman.esp 0xFE1..0xFE5
//     (Util::sizeKeyIDs).
//
// The index is not portable on its own - it means a different addon in a
// different install, which is why the payload has always stored a FormKey - but
// resolving it *here* is straightforward, because TNG builds its master list by
// walking the armour array in order and keeping everything carrying the
// male/female addon keyword (Core::LoadAddons). The same walk in the same
// session yields the same list.
//
// Both are synchronous reads of loaded forms, so there is nothing to wait for
// and nothing to time out.
// ═══════════════════════════════════════════════════════════════════════════

/// TheNewGentleman.esp keyword ids, from `Common::Util::keyIDs` and
/// `Common::Util::sizeKeyIDs`. Hard-coded because they are record ids in a
/// specific file rather than anything derivable.
constexpr std::string_view kTngPlugin = "TheNewGentleman.esp";
constexpr RE::FormID kKeywordAddonMale = 0xFF9;
constexpr RE::FormID kKeywordAddonFemale = 0xFFA;
constexpr RE::FormID kKeywordSizeFirst = 0xFE1;
constexpr int32_t kSizeCategoryCount = 5;

constexpr std::string_view kAutoAddonPrefix = "TNG_ActorAddnAuto:";
constexpr std::string_view kUserAddonPrefix = "TNG_ActorAddnUser:";

struct NativeState {
    bool haveAddon = false;
    /// Empty with `haveAddon` set means "TNG tracks this actor and it has no
    /// addon", which is a different answer from "we could not tell".
    std::string addonKey;
    int32_t addonIndex = -1;
    bool userChosen = false;
    bool haveSize = false;
    int32_t size = 0;
};

RE::BGSKeyword* TngKeyword(RE::FormID localId) {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        return nullptr;
    }
    return handler->LookupForm<RE::BGSKeyword>(localId, kTngPlugin);
}

/// TNG's master addon list for one gender, rebuilt the way `Core::LoadAddons`
/// builds it: the armour array in its own order, filtered by the addon keyword.
const std::vector<RE::TESObjectARMO*>& AddonList(bool female) {
    static std::vector<RE::TESObjectARMO*> male;
    static std::vector<RE::TESObjectARMO*> fem;
    static bool built = false;
    if (!built) {
        built = true;
        auto* handler = RE::TESDataHandler::GetSingleton();
        auto* maleKey = TngKeyword(kKeywordAddonMale);
        auto* femKey = TngKeyword(kKeywordAddonFemale);
        if (handler && (maleKey || femKey)) {
            for (auto* armor : handler->GetFormArray<RE::TESObjectARMO>()) {
                if (!armor) {
                    continue;
                }
                if (maleKey && armor->HasKeyword(maleKey)) {
                    male.push_back(armor);
                }
                if (femKey && armor->HasKeyword(femKey)) {
                    fem.push_back(armor);
                }
            }
        }
        spdlog::info("NpcTng: rebuilt TNG's addon lists natively - {} male, {} female",
                     male.size(), fem.size());
    }
    return female ? fem : male;
}

std::optional<int32_t> ParseAddonIndexFromKeywords(RE::TESNPC* npc, bool& userChosen) {
    std::optional<int32_t> found;
    bool user = false;
    npc->ForEachKeyword([&](RE::BGSKeyword* keyword) {
        const char* editorId = keyword ? keyword->GetFormEditorID() : nullptr;
        if (!editorId) {
            return RE::BSContainer::ForEachResult::kContinue;
        }
        const std::string_view text(editorId);
        std::string_view digits;
        if (text.starts_with(kAutoAddonPrefix)) {
            digits = text.substr(kAutoAddonPrefix.size());
        } else if (text.starts_with(kUserAddonPrefix)) {
            digits = text.substr(kUserAddonPrefix.size());
            user = true;
        } else {
            return RE::BSContainer::ForEachResult::kContinue;
        }
        // TNG writes it zero-padded to two digits; `from_chars` rather than
        // `stoi` so a malformed keyword is a miss rather than an exception on
        // the game thread.
        int32_t value = 0;
        const auto result =
            std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (result.ec == std::errc{}) {
            found = value;
            return RE::BSContainer::ForEachResult::kStop;
        }
        return RE::BSContainer::ForEachResult::kContinue;
    });
    userChosen = user;
    return found;
}

NativeState ReadNativeState(RE::Actor* actor) {
    NativeState state;
    auto* npc = actor ? actor->GetActorBase() : nullptr;
    if (!npc) {
        return state;
    }

    // ── Size ──────────────────────────────────────────────────────────────
    for (int32_t i = 0; i < kSizeCategoryCount; ++i) {
        auto* keyword = TngKeyword(kKeywordSizeFirst + static_cast<RE::FormID>(i));
        if (keyword && npc->HasKeyword(keyword)) {
            state.haveSize = true;
            state.size = i;
            break;
        }
    }

    // ── Addon ─────────────────────────────────────────────────────────────
    const auto index = ParseAddonIndexFromKeywords(npc, state.userChosen);
    if (!index) {
        return state;
    }
    state.addonIndex = *index;
    if (*index < 0) {
        // TNG's own "no addon" sentinel. A real answer, so `haveAddon` is set
        // with an empty key.
        state.haveAddon = true;
        return state;
    }
    const auto& list = AddonList(npc->IsFemale());
    if (static_cast<size_t>(*index) >= list.size()) {
        spdlog::warn("NpcTng: keyword names addon index {} but only {} are installed for this "
                     "gender; leaving the addon uncaptured",
                     *index, list.size());
        return state;
    }
    state.haveAddon = true;
    state.addonKey = Model::FormKeyUtil::BuildFormKey(list[static_cast<size_t>(*index)]);
    return state;
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

void NpcTng::PrepareCollect(RE::PlayerCharacter* player,
                            const std::vector<Model::ActorSubject>&) {
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
    // Only the player, and this is worth stating with evidence rather than as an
    // assertion, because it is the obvious thing to get wrong: you *can* assign an
    // addon to an NPC in game (the NPCEdit hotkey, and the MCM), so the natural
    // assumption is that the choice is per-save and needs carrying.
    //
    // It is not. Verified against TheNewGentleman.dll and a live
    // TheNewGentleman5.ini on 2026-08-10:
    //
    //   - The only save-scoped thing TNG stores is the *player*. Its INI layer
    //     exposes exactly one loader that takes a save id -
    //     `Inis::LoadPlayerInfos(const std::string&)` - and the sections it writes
    //     are named `[PlayerInfo<8 hex digits>]`, where those digits are the save
    //     line's id. The observed file held `[PlayerInfo00000000]`,
    //     `[PlayerInfo24CB5312]` and `[PlayerInfoE223B073]`, the last matching the
    //     `Save8_E223B073_...` file beside it.
    //   - NPC choices go to flat, unscoped `[NPCGenitalAddon]` and
    //     `[NPCGenitalSize]` sections keyed by the TESNPC base, e.g.
    //     `0x14120~Skyrim.esm = 0xd77~SOS - TRX - Futanari Addon_NG.esp`. Nothing
    //     in the section name varies by save.
    //   - TNG writes **no SKSE co-save record at all**. Scanning a 4.9 MB co-save
    //     from this playthrough found `TheNewGentleman` only inside plugin
    //     load-order lists, never as a record owner.
    //
    // So an in-game NPC assignment is written to a global file and applies to
    // every save on the install, including one started tomorrow. Migrating it
    // would mean writing a value that is already correct.
    //
    // The one genuine gap is an addon assigned to a *dynamically spawned* actor:
    // TNG's setter takes the reference as well as the base, and a `0xFF` reference
    // id is an allocator value private to one save. That is unmigratable by
    // construction, for the same reason the roster refuses dynamic refs, and
    // carrying it would resolve to an unrelated object.
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

    // The native read is the primary source; the VM answer is the cross-check.
    // That is the way round it should always have been - the native read cannot
    // time out - and having both is worth keeping, because a disagreement is the
    // only evidence that would show the keyword-and-index reconstruction has
    // drifted from what TNG itself would say.
    const auto native = ReadNativeState(subject.actor);
    payload["source"] = "keywords";

    if (native.haveAddon || native.haveSize) {
        if (haveAddon && native.haveAddon && addonKey != native.addonKey) {
            spdlog::warn(
                "NpcTng: the keyword read says addon '{}' (index {}) but TNG's own Papyrus call "
                "says '{}'. Recording TNG's answer and keeping the keyword one alongside it.",
                native.addonKey.empty() ? "(none)" : native.addonKey, native.addonIndex,
                addonKey.empty() ? "(none)" : addonKey);
            payload["addonFromKeywords"] = native.addonKey;
            payload["source"] = "papyrus";
        } else if (native.haveAddon) {
            addonKey = native.addonKey;
            haveAddon = true;
        }
        if (native.haveSize && !haveSize) {
            size = native.size;
            haveSize = true;
        }
        // Useful to the applier as a first guess, and it is only ever a guess -
        // the applier still reads back and sweeps if the index landed wrong.
        if (native.addonIndex >= 0) {
            payload["addonIndex"] = native.addonIndex;
        }
        payload["addonChosenByUser"] = native.userChosen;
    } else if (haveAddon || haveSize) {
        payload["source"] = "papyrus";
    }

    if (!haveAddon && !haveSize) {
        // Neither route produced anything. With the keyword read in place this
        // no longer means "the VM was busy" - it means TNG has never touched
        // this actor, which for the player is a genuine "no addon".
        payload["capturePending"] = true;
        payload["note"] =
            "Neither TNG's keywords nor its Papyrus surface reported an addon or a size for this "
            "actor. TNG most likely never processed it - check that the race is supported and that "
            "TheNewGentleman.esp is active.";
        ctx.report.Failed(Report::PlayerSubject(), std::format("{}/tng", subject.refKey),
                          Report::ReasonCode::kModApiMissing,
                          "TNG reported no state for this actor; addon and size not recorded",
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
    // No explanatory `note` here. It was the same constant sentence in every
    // payload of every export, describing why the addon is stored as a FormKey -
    // which is a fact about this code, not about the save, and is already said
    // where it belongs, at the top of this file. The conditional note above stays:
    // that one reports something that actually happened.

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
