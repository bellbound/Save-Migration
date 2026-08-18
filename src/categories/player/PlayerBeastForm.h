#pragma once

#include "core/Category.h"

namespace SaveMigration::Categories {

/// Werewolf and vampire perk trees, their banked points, and lycanthropy itself.
///
/// **Lycanthropy is read from the spell, never the race.** The werewolf race is
/// only worn while transformed, so a race check calls every lycanthrope who is
/// currently walking around as themselves "not a werewolf" - which is nearly all
/// of them, nearly all of the time. Beast Form is carried permanently and is the
/// honest marker.
///
/// Restoring it is just the two spells: Beast Form and the werewolf blood
/// passive. The quest stage and the race swap are consequences of transforming,
/// which the restored power produces by itself; there is nothing to synthesise.
///
/// **Vampire Lord is Dawnguard's, and a different arrangement.** Lycanthropy is
/// one power; a vampire lord is a disease that turns you and a form granted on
/// top of it, so Sanguinare Vampiris goes on first and the form after.
///
/// The three in-game days between them are **served, not skipped**: the clock is
/// pushed past the incubation and the apply pass then hands frames back to the
/// game until the quest is seen to take Sanguinare off the character again. The
/// turn is observed rather than timed, so a vampire overhaul that changes the
/// schedule does not silently break it - and the wait ends the moment it lands.
///
/// That is why it is experimental and off by default: moving the clock fires
/// every armed game-time timer in the load order at once, exactly as
/// `GameClock`'s mode 2 warns. Three days of crops, respawns, pregnancies and
/// bounties go by. It is a real cost, taken deliberately, and reported.
///
/// **Vampire Lord is applied before lycanthropy, always.** The engine treats the
/// two as mutually exclusive and whichever lands last wins, so for a character
/// who was both, that order is the entire difference between getting both back
/// and getting only the wolf.
///
/// Werewolf feed count has no accessor in this CommonLib fork. It is simply
/// absent; a field that cannot be read is not worth a line in every report.
class PlayerBeastForm final : public Core::IGlobalCategory {
public:
    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;

private:
    static void RestoreLycanthropy(Core::ApplyContext& ctx, const Report::SubjectRef& subject);
    /// Grants Sanguinare Vampiris, pushes the clock past its incubation, and then
    /// hands frames back until the character actually turns.
    void RestoreVampireLord(Core::ApplyContext& ctx, const Report::SubjectRef& subject);
    void ResumeVampireLord(Core::ApplyContext& ctx, const Report::SubjectRef& subject);
    void FinishVampireLord(Core::ApplyContext& ctx, const Report::SubjectRef& subject, bool turned);

    /// Set while the apply pass is giving frames back to the game so the disease
    /// can run. `Apply` re-enters on each of them and must not redo its own work.
    bool m_waitingForTurn = false;
    uint32_t m_waitedFrames = 0;
    /// Held across the wait: the wolf must not go on until the vampire has.
    bool m_pendingWerewolf = false;
};

}  // namespace SaveMigration::Categories
