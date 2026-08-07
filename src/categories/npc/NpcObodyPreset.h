#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// OBody NG body presets.
///
/// **Not installed in this load order**, so in practice this degrades to one info
/// line. It is implemented anyway because the gotcha is subtle enough to be worth
/// capturing while it is understood.
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
/// - `obody_ng_distribution_key` is **preserved**, because that key is what stops
///   OBody re-randomising everyone it has already seen.
/// - The NiOverride morphs themselves are not snapshotted: they are derived from the
///   preset and regenerable, so carrying them would be redundant data that can
///   disagree with its own source.
/// - `MarkForReprocess` is never called; it invites exactly the redistribution the
///   distribution key exists to prevent.
class NpcObodyPreset final : public Core::IActorCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void BeginCollect(Core::CollectContext& ctx) override;
    void CollectActor(const Model::ActorSubject& subject, Core::CollectContext& ctx) override;
    void ApplyActor(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;
    bool ApplyDeferred(const Model::ActorSubject& subject, Core::ApplyContext& ctx) override;

    /// `obody_<signed decimal form id>_preset`. Exposed for the unit-testable shape
    /// of the rule, and because getting it wrong is silent.
    [[nodiscard]] static std::string PresetKeyFor(RE::FormID formId);
};

}  // namespace SaveMigration::Categories
