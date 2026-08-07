#include "core/MigrationState.h"

#include <format>

namespace SaveMigration::Core {

MigrationState& MigrationState::Get() {
    static MigrationState instance;
    return instance;
}

void MigrationState::Save(SKSE::SerializationInterface* intfc) {
    if (!intfc->WriteRecordData(&m_flags, sizeof(m_flags))) {
        spdlog::error("MigrationState: failed to write flags");
        return;
    }
    if (!SerializationHub::WriteString(intfc, m_restoredFrom)) {
        spdlog::error("MigrationState: failed to write restoredFrom");
        return;
    }
    if (!intfc->WriteRecordData(&m_restoredAtGameTime, sizeof(m_restoredAtGameTime))) {
        spdlog::error("MigrationState: failed to write restoredAtGameTime");
    }
}

bool MigrationState::Load(SKSE::SerializationInterface* intfc, uint32_t, uint32_t) {
    uint32_t flags = 0;
    if (!intfc->ReadRecordData(&flags, sizeof(flags))) {
        return false;
    }
    std::string restoredFrom;
    if (!SerializationHub::ReadString(intfc, restoredFrom, 256)) {
        return false;
    }
    float gameTime = 0.0f;
    if (!intfc->ReadRecordData(&gameTime, sizeof(gameTime))) {
        return false;
    }

    m_flags = flags;
    m_restoredFrom = std::move(restoredFrom);
    m_restoredAtGameTime = gameTime;

    spdlog::info(
        "MigrationState: flags=0x{:X} (newGame={}, applied={}, declined={}, inProgress={}), from='{}'",
        m_flags, HasFlag(StateFlag::kSeenNewGame), HasFlag(StateFlag::kRestoreApplied),
        HasFlag(StateFlag::kRestoreDeclined), HasFlag(StateFlag::kRestoreInProgress),
        m_restoredFrom);
    return true;
}

void MigrationState::Revert() {
    m_flags = 0;
    m_restoredFrom.clear();
    m_restoredAtGameTime = 0.0f;
}

bool MigrationState::HasFlag(StateFlag flag) const {
    return (m_flags & static_cast<uint32_t>(flag)) != 0;
}

void MigrationState::SetFlag(StateFlag flag) {
    m_flags |= static_cast<uint32_t>(flag);
    spdlog::debug("MigrationState: set flag 0x{:X}, now 0x{:X}", static_cast<uint32_t>(flag),
                  m_flags);
}

void MigrationState::ClearFlag(StateFlag flag) {
    m_flags &= ~static_cast<uint32_t>(flag);
}

void MigrationState::MarkRestored(std::string snapshotId, float gameTimeDays) {
    m_restoredFrom = std::move(snapshotId);
    m_restoredAtGameTime = gameTimeDays;
    ClearFlag(StateFlag::kRestoreInProgress);
    SetFlag(StateFlag::kRestoreApplied);
    spdlog::info("MigrationState: restore applied from '{}' at game day {:.4f}", m_restoredFrom,
                 gameTimeDays);
}

void MigrationState::ClearRestoreDecision() {
    ClearFlag(StateFlag::kRestoreApplied);
    ClearFlag(StateFlag::kRestoreDeclined);
    ClearFlag(StateFlag::kRestoreInProgress);
    spdlog::warn("MigrationState: restore decision cleared by debug request");
}

}  // namespace SaveMigration::Core
