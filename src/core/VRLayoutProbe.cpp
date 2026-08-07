#include "core/VRLayoutProbe.h"

#include <format>
#include <string>

namespace SaveMigration::Core {

namespace {
/// A player can legitimately have a few hundred map markers in flight. Anything
/// past this is a misread rather than a big save.
constexpr uint32_t kMaxPlausibleMapMarkers = 8192;
}  // namespace

VRLayoutProbe& VRLayoutProbe::Get() {
    static VRLayoutProbe instance;
    return instance;
}

void VRLayoutProbe::Probe() {
    if (m_probed) {
        return;
    }
    m_probed = true;

    if (!REL::Module::IsVR()) {
        m_trusted = true;
        m_detail = "non-VR runtime; VR offset block not used";
        spdlog::info("VRLayoutProbe: {}", m_detail);
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        m_trusted = false;
        m_detail = "PlayerCharacter singleton unavailable at probe time";
        spdlog::error("VRLayoutProbe: E_RUNTIME_LAYOUT_SUSPECT - {}", m_detail);
        return;
    }

    // The single field in VR_PLAYER_RUNTIME_DATA that the header marks as
    // confirmed. If its size/capacity/data triple is internally inconsistent we
    // are reading the wrong address.
    const auto& markers = player->GetVRPlayerRuntimeData().currentMapMarkers;
    const uint32_t size = markers.size();
    const uint32_t capacity = markers.capacity();
    const void* data = markers.data();

    const bool sizeSane = size <= kMaxPlausibleMapMarkers;
    const bool capacitySane = capacity <= kMaxPlausibleMapMarkers && capacity >= size;
    const bool pointerSane = (size == 0) || (data != nullptr);

    if (sizeSane && capacitySane && pointerSane) {
        m_trusted = true;
        m_detail = std::format("currentMapMarkers consistent (size={}, capacity={})", size, capacity);
        spdlog::info("VRLayoutProbe: VR layout trusted - {}", m_detail);
        return;
    }

    m_trusted = false;
    m_detail = std::format(
        "currentMapMarkers inconsistent (size={}, capacity={}, data={}) - offset-dependent readers disabled",
        size, capacity, data ? "non-null" : "null");
    spdlog::error("VRLayoutProbe: E_RUNTIME_LAYOUT_SUSPECT - {}", m_detail);
}

RE::PlayerCharacter::PlayerSkills* PlayerSkillsOf(RE::PlayerCharacter* player) {
    if (!player) {
        return nullptr;
    }
    if (auto* vr = player->GetVRInfoRuntimeData()) {
        return vr->skills;
    }
    return player->GetInfoRuntimeData().skills;
}

RE::PlayerCharacter::PlayerSkills::Data* PlayerSkillDataOf(RE::PlayerCharacter* player) {
    auto* skills = PlayerSkillsOf(player);
    return skills ? skills->data : nullptr;
}

}  // namespace SaveMigration::Core
