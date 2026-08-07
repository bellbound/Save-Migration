#pragma once

namespace SaveMigration::Core {

/// Guards every read that depends on a hard-coded struct offset into
/// `PlayerCharacter`.
///
/// Background: the two vendored CommonLib forks in this workspace disagree by
/// eight bytes on the base of `VR_PLAYER_RUNTIME_DATA`, and the header itself
/// annotates `addedPerks` (0xAA0), `perks` (0xAB8) and `standingStonePerks`
/// (0xAD0) as guesses. Exactly one field in that block carries a "confirmed"
/// annotation: `currentMapMarkers` (0xAE8). So we probe that one, and if it
/// looks wrong we assume the whole block is misaligned and refuse to read any
/// of it for the session.
///
/// This is why the perk categories enumerate `GetFormArray<BGSPerk>()` and test
/// `Actor::HasPerk` instead of walking the perk arrays: a relocated engine call
/// is offset-immune, so it stays correct even when the probe fails.
class VRLayoutProbe {
public:
    static VRLayoutProbe& Get();

    /// Run once at kDataLoaded. Safe to call again; only the first call probes.
    void Probe();

    /// False once the probe has failed. Offset-dependent readers must consult
    /// this and downgrade to `runtime_layout_suspect` rather than reading.
    [[nodiscard]] bool IsLayoutTrusted() const { return m_trusted; }

    [[nodiscard]] bool HasProbed() const { return m_probed; }

    /// Human-readable outcome, for the report header.
    [[nodiscard]] const std::string& Detail() const { return m_detail; }

private:
    VRLayoutProbe() = default;

    bool m_probed = false;
    bool m_trusted = true;
    std::string m_detail;
};

/// The one sanctioned way to reach `PlayerSkills`.
///
/// `PlayerCharacter::GetInfoRuntimeData()` resolves to
/// `RelocateMember<INFO_RUNTIME_DATA>(this, 0x8E4, /*VR*/ 0)` — a VR offset of
/// zero means *unsupported*, and dereferencing it reads from the object header.
/// On VR the skills live behind `GetVRInfoRuntimeData()` at 0xFE0 instead.
[[nodiscard]] RE::PlayerCharacter::PlayerSkills* PlayerSkillsOf(RE::PlayerCharacter* player);

/// Convenience: the skill data block, or nullptr if unavailable.
[[nodiscard]] RE::PlayerCharacter::PlayerSkills::Data* PlayerSkillDataOf(
    RE::PlayerCharacter* player);

}  // namespace SaveMigration::Core
