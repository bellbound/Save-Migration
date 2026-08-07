#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace SaveMigration::Model {

/// Stable, load-order-independent form identity: `"0x{LocalFormID:X}~{Plugin.esm}"`.
///
/// Copied from VR-Sex-Menu's `Persistence::FormKeyUtil`. `BuildFormKey` and
/// `ParseFormKey` are unchanged — they were already correct. Resolution is
/// **not**: the original synthesised the runtime FormID by hand and branched on
/// `TESFile::IsLight()`, which reads the plugin's own `kSmallFile` header flag
/// regardless of whether the *runtime* supports ESLs. Skyrim VR 1.4.15 has no
/// ESL space at all, so an ESL-flagged plugin there gets `compileIndex << 24`,
/// not `0xFE000000 | (smallFileIndex << 12)` — and the hand-rolled version
/// produced a garbage ID that silently resolved to nothing or, worse, to an
/// unrelated form.
///
/// Everything now routes through `TESDataHandler::LookupForm`, which contains
/// the `REL::Module::IsVR()` branch internally. No call site in this plugin may
/// call `TESForm::LookupByID` on a reconstructed ID.
class FormKeyUtil {
public:
    /// Empty string if `form` is null or has no source file (a dynamic
    /// `0xFF……` form created at runtime). An empty key is never written to disk
    /// as a `form` field — the caller routes it to `unmigratable[]` with reason
    /// `dynamic_form`, because a dynamic ID means nothing in another save.
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
