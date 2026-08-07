#pragma once

#include <string>

#include "core/SerializationHub.h"

namespace SaveMigration::Core {

/// Per-playthrough identity, minted into the co-save.
///
/// The id is `"<epoch_ms>-<rand 0..999999>"`, the same shape SkyrimNet uses
/// (`skse/SkyrimNet/src/Skyrim/plugin.cpp`), so the two plugins' snapshot
/// directories are recognisably siblings and a user can match them by eye.
///
/// Why a co-save id rather than the save filename: the filename changes on
/// every quicksave and is renamed freely by the user, while the co-save value
/// is copied forward by the engine on save-over-save and therefore identifies
/// the *playthrough*.
class SaveIdentity final : public IRecordHandler {
public:
    static constexpr uint32_t kSignature = MakeSig('S', 'M', 'I', 'D');
    static constexpr uint32_t kVersion = 1;

    static SaveIdentity& Get();

    [[nodiscard]] uint32_t Signature() const override { return kSignature; }
    [[nodiscard]] uint32_t Version() const override { return kVersion; }
    [[nodiscard]] const char* Name() const override { return "SMID"; }

    void Save(SKSE::SerializationInterface* intfc) override;
    bool Load(SKSE::SerializationInterface* intfc, uint32_t version, uint32_t length) override;
    void Revert() override;
    void PostLoad(bool wasPresent) override;

    /// Current playthrough id. Never empty once a game is loaded or started.
    [[nodiscard]] const std::string& SaveId() const { return m_saveId; }

    /// True when an SMID record was actually read from the co-save just loaded.
    /// False means the save predates this plugin — which, combined with
    /// `kSeenNewGame`, is what identifies "a new playthrough that has been
    /// saved and reloaded".
    [[nodiscard]] bool WasFoundInCoSave() const { return m_foundInCoSave; }

    /// True once SKSE has issued a revert for this session, i.e. an actual
    /// savegame load happened rather than us sitting at first boot.
    [[nodiscard]] bool HasReverted() const { return m_hasReverted; }

    /// Called on kNewGame so a brand-new game still has an id to key a
    /// snapshot directory with.
    void EnsureId();

    static std::string GenerateUniqueID();

private:
    SaveIdentity() = default;

    std::string m_saveId;
    bool m_foundInCoSave = false;
    bool m_hasReverted = false;
};

}  // namespace SaveMigration::Core
