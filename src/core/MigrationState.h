#pragma once

#include <string>

#include "core/SerializationHub.h"

namespace SaveMigration::Core {

enum class StateFlag : uint32_t {
    /// This playthrough was started as a new game while the plugin was loaded.
    kSeenNewGame = 1u << 0,
    /// A restore has completed against this save line. This is the *only*
    /// suppressor for "never runs twice".
    kRestoreApplied = 1u << 1,
    // 1u << 2 was kRestoreDeclined, set by a load-time prompt that no longer
    // exists. The bit is deliberately not reused: co-saves written by earlier
    // builds still carry it, and giving it a new meaning would make those saves
    // assert something they never said.
    /// A restore was in flight when the game was saved — used to avoid
    /// snapshotting a half-restored world.
    kRestoreInProgress = 1u << 3,
};

/// 'SMST' — the per-save decision record.
///
/// The scope of these flags is deliberate. Living in the co-save means they
/// travel with the save, survive quickload, and are *absent* from a genuinely
/// pre-restore save — so loading backwards past a restore correctly leaves the
/// save importable again.
class MigrationState final : public IRecordHandler {
public:
    static constexpr uint32_t kSignature = MakeSig('S', 'M', 'S', 'T');
    static constexpr uint32_t kVersion = 1;

    static MigrationState& Get();

    [[nodiscard]] uint32_t Signature() const override { return kSignature; }
    [[nodiscard]] uint32_t Version() const override { return kVersion; }
    [[nodiscard]] const char* Name() const override { return "SMST"; }

    void Save(SKSE::SerializationInterface* intfc) override;
    bool Load(SKSE::SerializationInterface* intfc, uint32_t version, uint32_t length) override;
    void Revert() override;

    [[nodiscard]] bool HasFlag(StateFlag flag) const;
    void SetFlag(StateFlag flag);
    void ClearFlag(StateFlag flag);

    [[nodiscard]] const std::string& RestoredFromSnapshotId() const { return m_restoredFrom; }
    [[nodiscard]] float RestoredAtGameTime() const { return m_restoredAtGameTime; }

    void MarkRestored(std::string snapshotId, float gameTimeDays);

    /// Make this save line importable again.
    ///
    /// `kRestoreApplied` is what stops a snapshot being applied twice, so this is
    /// the escape hatch for "the import went wrong, let me try again" - reachable
    /// from the menu's Advanced page rather than by hand-editing a co-save.
    void ClearAppliedDecision();

private:
    MigrationState() = default;

    uint32_t m_flags = 0;
    std::string m_restoredFrom;
    float m_restoredAtGameTime = 0.0f;
};

}  // namespace SaveMigration::Core
