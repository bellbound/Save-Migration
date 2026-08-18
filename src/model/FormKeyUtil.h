#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace SaveMigration::Model {

/// Stable, load-order-independent form identity: `"0x{LocalFormID:X}~{Plugin.esm}"`.
///
/// Copied from VR-Sex-Menu's `Persistence::FormKeyUtil`, and rewritten at both
/// ends since. The one property the whole thing rests on: **building a key and
/// resolving it are exact inverses of each other.** Break that symmetry and a
/// snapshot records identities that name nothing, which the report then blames
/// on a missing mod.
///
/// Resolution goes through `TESDataHandler::LookupForm`, never a hand-built
/// runtime FormID. The original synthesised one and branched on
/// `TESFile::IsLight()`, which reads the plugin's own `kSmallFile` header flag
/// regardless of whether the *runtime* supports ESLs — and whether it does is
/// not a property of the game: stock Skyrim VR has no ESL space, SkyrimVRESL
/// gives VR one, and this workspace runs it. No call site in this plugin may
/// call `TESForm::LookupByID` on a reconstructed ID, and nothing here decides
/// the ESL question by asking which game is running.
///
/// Building reads the owning plugin off the FormID's own index bits, which is
/// that same branch run backwards. It does **not** ask the form which files it
/// came from: for an overridden record that list can name only the overriding
/// plugin, and pairing an override's name with an id it never declared produces
/// a key that cannot resolve anywhere.
class FormKeyUtil {
public:
    /// Empty string if `form` is null, is a dynamic `0xFF……` form created at
    /// runtime, or carries a load order index no loaded plugin holds. An empty
    /// key is never written to disk as a `form` field — the caller routes it to
    /// `unmigratable[]` with reason `dynamic_form`, because an ID we cannot
    /// attribute means nothing in another save. Silence is the right answer
    /// there: a key naming the wrong plugin is worse than no key, because the
    /// import reports it as the player's mod being gone.
    static std::string BuildFormKey(RE::TESForm* form);
    static std::string BuildFormKey(RE::TESObjectREFR* ref);
    static std::string BuildFormKey(RE::FormID localFormId, std::string_view pluginName);

    struct ParsedKey {
        RE::FormID localFormId{};
        std::string pluginName;
    };

    static std::optional<ParsedKey> ParseFormKey(std::string_view keyString);

    /// True if the plugin named in `keyString` is present in the current load
    /// order. Cheap pre-check so a category can pre-fail a whole plugin's worth
    /// of keys without attempting lookups.
    static bool IsPluginLoaded(std::string_view pluginName);

    /// Untyped resolution. Prefer `FormRef::ResolveChecked<T>`, which also
    /// verifies the form type — a load order change can put a different record
    /// type at the same local ID.
    static RE::TESForm* Resolve(std::string_view keyString);

private:
    static const RE::TESFile* FindFile(std::string_view pluginName);
};

}  // namespace SaveMigration::Model
