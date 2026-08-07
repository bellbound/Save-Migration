#pragma once

#include <cmath>

namespace SaveMigration::Util {

/// Reposition a reference into an arbitrary cell without teleporting anybody.
///
/// `TESObjectREFR::MoveTo_Impl` is `private` in CommonLib, so we re-declare it
/// against the public offset `Offset::TESObjectREFR::MoveTo`
/// (`RELOCATION_ID(56227, 56626)`). This is not a hack around an intentional
/// restriction — CommonLib's own public `SetPosition()` calls the same address
/// the same way, so the relocation is known-good on VR.
///
/// This primitive is what makes home restore *instant*. My Home Is Your Home
/// pre-places 250 persistent XMarkers in its ESP and "setting a home" just moves
/// marker *i* to the player. Being able to move those markers into arbitrary
/// cells directly means we never have to walk the player around the world to
/// re-place them.
///
/// The target handle is passed empty: MoveTo_Impl uses it only to derive a
/// destination when no explicit cell is supplied, and we always supply one.
inline bool MoveRefTo(RE::TESObjectREFR* ref, RE::TESObjectCELL* cell, RE::TESWorldSpace* worldSpace,
                      const RE::NiPoint3& position, const RE::NiPoint3& rotation) {
    if (!ref) {
        return false;
    }
    if (!cell && !worldSpace) {
        spdlog::warn("MoveRefTo: refusing to move {:08X} with neither cell nor worldspace",
                     ref->GetFormID());
        return false;
    }

    // A NaN or absurd coordinate reaching the engine is a hard crash inside the
    // cell attach path, so it is checked here rather than at each call site.
    for (const float component : {position.x, position.y, position.z}) {
        if (!std::isfinite(component) || std::abs(component) > 1.0e7f) {
            spdlog::warn("MoveRefTo: refusing non-finite/out-of-range position for {:08X}",
                         ref->GetFormID());
            return false;
        }
    }
    for (const float component : {rotation.x, rotation.y, rotation.z}) {
        if (!std::isfinite(component)) {
            spdlog::warn("MoveRefTo: refusing non-finite rotation for {:08X}", ref->GetFormID());
            return false;
        }
    }

    using func_t = void (*)(RE::TESObjectREFR*, const RE::ObjectRefHandle&, RE::TESObjectCELL*,
                            RE::TESWorldSpace*, const RE::NiPoint3&, const RE::NiPoint3&);
    static REL::Relocation<func_t> func{RE::Offset::TESObjectREFR::MoveTo};

    const RE::ObjectRefHandle emptyTarget{};
    func(ref, emptyTarget, cell, worldSpace, position, rotation);
    return true;
}

/// Convenience overload for markers, which carry no meaningful rotation.
inline bool MoveRefTo(RE::TESObjectREFR* ref, RE::TESObjectCELL* cell, RE::TESWorldSpace* worldSpace,
                      const RE::NiPoint3& position) {
    return MoveRefTo(ref, cell, worldSpace, position, RE::NiPoint3{0.0f, 0.0f, 0.0f});
}

}  // namespace SaveMigration::Util
