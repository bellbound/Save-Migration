#pragma once

// =============================================================================
// DressUp VR Plugin Interface v002
// =============================================================================
// This header is designed to be copied to other SKSE projects.
//
// v001 is FROZEN. Nothing in it may change: consumers compiled against it rely on
// its exact vtable layout. v002 is a separate interface returned by the same
// GetDressUpInterface() export when asked for version 2.
//
// v002 adds enumeration and FormKey-based injection, so an outfit set can be read
// out of one savegame and put into another without either side knowing runtime
// form IDs. That is what makes VR Dress Up outfits migratable at all.
//
// Thread Safety:
//   All API calls must be made from the game's main thread.
// =============================================================================

#include <cstdint>

namespace RE {
    class Actor;
}

namespace DressUp {

constexpr uint32_t DRESSUP_INTERFACE_VERSION_002 = 2;

// -----------------------------------------------------------------------------
// Plain-old-data string list.
//
// std::vector and std::string cannot cross a DLL boundary safely - the two sides
// may be built with different standard libraries or different iterator debug
// levels. So enumeration hands back a borrowed array of NUL-terminated pointers
// that stays valid until the next call on the same interface.
// -----------------------------------------------------------------------------
struct StringList {
    const char* const* items = nullptr;
    uint32_t count = 0;
};

struct Interface002 {
    // === Version ===
    virtual uint32_t GetVersion() = 0;
    virtual uint32_t GetBuild() = 0;

    // === Enumeration ===

    // Names of every outfit stored for `actor`, including the internal "locked" and
    // "default" entries. Borrowed; valid until the next call on this interface.
    virtual StringList EnumerateOutfits(RE::Actor* actor) = 0;

    // FormKeys of the armour items in one stored outfit, in stored order.
    // Empty when the outfit does not exist.
    virtual StringList EnumerateOutfitItems(RE::Actor* actor, const char* outfitName) = 0;

    // FormKeys of the items this actor was given by the player.
    virtual StringList EnumeratePlayerGivenItems(RE::Actor* actor) = 0;

    // === Injection ===

    // Replace (or create) `outfitName` for `actor` from a list of FormKeys.
    //
    // This writes the map only. It does **not** equip anything and does **not**
    // touch the actor's inventory, which is deliberate - see the warning on
    // ApplyOutfitNow.
    //
    // Returns the number of keys accepted. A key whose plugin is absent is skipped
    // and not counted.
    virtual uint32_t SetOutfitByFormKeys(RE::Actor* actor, const char* outfitName,
                                         const char* const* formKeys, uint32_t count) = 0;

    // Re-register items as player-given, by FormKey.
    virtual uint32_t MarkPlayerGivenByFormKeys(RE::Actor* actor, const char* const* formKeys,
                                               uint32_t count) = 0;

    // Ensure every item in a stored outfit is present in the actor's inventory,
    // adding any that are missing. Returns the number added.
    //
    // **This must be called before ApplyOutfitNow on a freshly injected outfit.**
    // ApplyOutfit prunes from the stored outfit any item the actor does not
    // currently have, and that pruning is permanent - it rewrites the map. Applying
    // an injected outfit against an empty inventory therefore does not merely fail
    // to dress the actor, it destroys the outfit you just injected.
    virtual uint32_t EnsureOutfitItemsInInventory(RE::Actor* actor, const char* outfitName) = 0;

    // Equip a stored outfit now. Same semantics as v001's internal apply, including
    // the pruning above - so call EnsureOutfitItemsInInventory first.
    virtual bool ApplyOutfitNow(RE::Actor* actor, const char* outfitName, bool unequipOthers) = 0;

    // === v001 functionality, repeated so a consumer needs only one interface ===
    virtual void SetOpenDressMenuEnabled(bool enabled) = 0;
    virtual bool IsOpenDressMenuEnabled() = 0;
    virtual bool LockActor(RE::Actor* actor) = 0;
    virtual bool UnlockActor(RE::Actor* actor) = 0;
    virtual bool IsActorLocked(RE::Actor* actor) = 0;

    // === Reserved ===
    virtual void _reserved1() {}
    virtual void _reserved2() {}
    virtual void _reserved3() {}
    virtual void _reserved4() {}
    virtual void _reserved5() {}
    virtual void _reserved6() {}
    virtual void _reserved7() {}
    virtual void _reserved8() {}
};

// =============================================================================
// Consumer accessor
// =============================================================================
//
//   #include "DressUpInterface002.h"
//   #include <Windows.h>
//
//   namespace DressUp {
//       typedef void* (*GetDressUpInterfaceFunc)(unsigned int version);
//       Interface002* GetInterface002() {
//           static Interface002* cached = nullptr;
//           if (cached) return cached;
//           HMODULE module = GetModuleHandleA("DressUpVR.dll");
//           if (!module) return nullptr;
//           auto fn = (GetDressUpInterfaceFunc)GetProcAddress(module, "GetDressUpInterface");
//           if (!fn) return nullptr;
//           cached = static_cast<Interface002*>(fn(2));
//           return cached;
//       }
//   }

}  // namespace DressUp
