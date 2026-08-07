#include "core/SaveIdentity.h"

#include <chrono>
#include <format>

#include <random>

namespace SaveMigration::Core {

SaveIdentity& SaveIdentity::Get() {
    static SaveIdentity instance;
    return instance;
}

std::string SaveIdentity::GenerateUniqueID() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    // Seeding from the same clock reading keeps this reproducible from a log
    // line, and two saves minted in the same millisecond are already
    // distinguished by the millisecond value itself in practice.
    std::mt19937 rng(static_cast<unsigned int>(ms));
    std::uniform_int_distribution<int> dist(0, 999999);

    auto id = std::format("{}-{}", ms, dist(rng));
    spdlog::info("SaveIdentity: minted new save id {}", id);
    return id;
}

void SaveIdentity::EnsureId() {
    if (m_saveId.empty()) {
        m_saveId = GenerateUniqueID();
    }
}

void SaveIdentity::Save(SKSE::SerializationInterface* intfc) {
    EnsureId();
    if (!SerializationHub::WriteString(intfc, m_saveId)) {
        spdlog::error("SaveIdentity: failed to write save id");
        return;
    }
    spdlog::debug("SaveIdentity: wrote save id {}", m_saveId);
}

bool SaveIdentity::Load(SKSE::SerializationInterface* intfc, uint32_t, uint32_t) {
    std::string loaded;
    if (!SerializationHub::ReadString(intfc, loaded, 128)) {
        return false;
    }
    m_saveId = std::move(loaded);
    spdlog::info("SaveIdentity: loaded save id {}", m_saveId);
    return true;
}

void SaveIdentity::Revert() {
    m_saveId.clear();
    m_foundInCoSave = false;
    // Deliberately *not* reset: the flag records that a load happened at all
    // this session, and SKSE issues the revert as part of that load.
    m_hasReverted = true;
}

void SaveIdentity::PostLoad(bool wasPresent) {
    m_foundInCoSave = wasPresent;
    if (!wasPresent) {
        spdlog::info("SaveIdentity: no SMID in co-save - this save predates Save Migration");
    }
    // Mint unconditionally afterwards so a snapshot taken on a pre-plugin save
    // still has somewhere to go. The id then persists from the next save on.
    EnsureId();
}

}  // namespace SaveMigration::Core
