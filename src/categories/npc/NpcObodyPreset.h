#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "core/Category.h"

namespace SaveMigration::Categories {

/// OBody NG body presets, for the player and every rostered NPC.
///
/// The player is included on purpose and needs no special case: the walk hands
/// every per-actor category the player first and the roster after, and the key
/// rule below is the same for both - the player is simply form ID 20.
///
/// **The gotcha.** OBody's per-actor StorageUtil key is built in Papyrus as
/// `"obody_" + akActor.GetFormID() + "_preset"`, and Papyrus stringifies an Int in
/// **signed decimal**, not hex. The player is therefore `"obody_20_preset"`, not
/// `"obody_14_preset"`. From C++ that means
/// `std::format("obody_{}_preset", static_cast<int32_t>(formID))` - the signed cast
/// matters, because a form ID above 0x7FFFFFFF stringifies negative in Papyrus.
/// Get it wrong and the read and the write both silently address nothing.
///
/// Two deliberate omissions:
/// - `obody_ng_distribution_key` is recorded but **never written back**. It is what
///   stops OBody re-randomising everyone it has already seen, and writing another
///   mod's global state on an inference is how a redistribution gets triggered.
///   Carrying the value means a future build can act on it without a fresh export.
/// - The NiOverride morphs themselves are not snapshotted: they are derived from the
///   preset and regenerable, so carrying them would be redundant data that can
///   disagree with its own source.
/// - `MarkForReprocess` is never called; it invites exactly the redistribution the
///   distribution key exists to prevent.
///
/// **Why the reads are primed.** OBody stores through PapyrusUtil's StorageUtil,
/// which is only reachable by a Papyrus call, and the harvest is a single
/// game-thread task - so a call dispatched from `CollectActor` cannot possibly
/// answer before the document is written. An earlier revision did exactly that,
/// with callbacks that only logged, so **no preset was ever recorded for anyone,
/// the player included**, and the applier's first line found an empty value and
/// returned. Every read now happens in `PrepareCollect`, `iVmSettleDelayMs`
/// ahead of the harvest, and the collector reports only what actually came back.
class NpcObodyPreset final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void PrepareCollect(RE::PlayerCharacter* player,
                        const std::vector<Model::ActorSubject>& roster) override;
    void BeginCollect(Core::CollectContext& ctx) override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void EndCollect(Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    bool ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;

    /// `obody_<signed decimal form id>_preset`. Exposed for the unit-testable shape
    /// of the rule, and because getting it wrong is silent.
    [[nodiscard]] static std::string PresetKeyFor(RE::FormID formId);

private:
    /// Answers that came back from the VM between `PrepareCollect` and the
    /// harvest. Keyed by form ID rather than by FormKey so it still matches when
    /// the prime roster and the harvest roster disagree about an actor.
    ///
    /// Written from the VM callback thread, read on the game thread, hence the
    /// mutex. Absence means "the VM did not answer", which is reported as such -
    /// deliberately distinct from "answered, and the actor has no preset".
    struct Primed {
        std::mutex mutex;
        std::unordered_map<RE::FormID, std::string> presets;
        std::string distributionKey;
        bool haveDistributionKey = false;
    };
    Primed m_primed;

    /// Actors OBody answered for and had nothing stored about. Summarised once in
    /// `EndCollect` rather than reported per actor: on a roster OBody has never
    /// rendered, that was one identical `partial_by_design` line per NPC.
    uint32_t m_noPresetCount = 0;

    void ResetPrimed();
};

}  // namespace SaveMigration::Categories
