#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "model/FormKeyUtil.h"
#include "report/MigrationReport.h"

namespace SaveMigration::Model {

/// A form as it appears on disk: the stable key plus a human-readable name so a
/// report can name something that no longer resolves.
struct FormRef {
    std::string key;          // "0x13BBF~Skyrim.esm", or empty when dynamic
    std::string displayName;  // best-effort, for reports only

    [[nodiscard]] bool IsEmpty() const { return key.empty(); }

    static FormRef From(RE::TESForm* form);
    static FormRef From(RE::TESObjectREFR* ref);
};

/// Outcome of a resolution attempt, so the caller can attribute a failure
/// without re-deriving why.
struct ResolveResult {
    RE::TESForm* form = nullptr;
    Report::ReasonCode reason = Report::ReasonCode::kNone;

    [[nodiscard]] explicit operator bool() const { return form != nullptr; }
};

/// Session-scoped resolution service.
///
/// Owns the plugin alias map so a renamed plugin's keys still resolve, and is
/// the only place in the plugin permitted to turn a stored key back into a
/// form. `ResolveChecked<T>` additionally verifies the record type: a plugin
/// update can put a different record at the same local ID, and a silent
/// mis-cast there is a crash rather than a failed migration.
class FormResolver {
public:
    static FormResolver& Get();

    /// `spec` is the INI `sPluginAliases` value: `Old.esp=New.esp,Other.esp=Third.esp`.
    void SetAliases(std::string_view spec);

    /// Plugins named in a snapshot that are absent from this load order.
    /// Precomputed once per restore so keys from them are pre-failed with
    /// `source_plugin_missing` and never reach a lookup.
    void SetMissingPlugins(std::vector<std::string> plugins);
    [[nodiscard]] const std::vector<std::string>& MissingPlugins() const { return m_missing; }
    [[nodiscard]] bool IsPluginKnownMissing(std::string_view pluginName) const;

    /// Resolve without a type check. Prefer the typed form.
    [[nodiscard]] ResolveResult Resolve(std::string_view key) const;

    /// Resolve and require the result to actually be a `T`.
    ///
    /// Uses `TESForm::As<T>()` rather than a bare `Is(T::FORMTYPE)` comparison:
    /// `As` is a form-type dispatch table that accepts legitimate subclasses,
    /// so `As<TESObjectREFR>()` on an `ActorCharacter` succeeds where an exact
    /// FORMTYPE equality test would wrongly reject it.
    template <class T>
    [[nodiscard]] T* ResolveChecked(std::string_view key, Report::ReasonCode& outReason) const {
        const auto result = Resolve(key);
        outReason = result.reason;
        if (!result.form) {
            return nullptr;
        }
        auto* typed = result.form->As<T>();
        if (!typed) {
            outReason = Report::ReasonCode::kFormTypeChanged;
            spdlog::warn("FormResolver: '{}' resolved to form type {} which is not the expected type",
                         key, static_cast<int>(result.form->GetFormType()));
            return nullptr;
        }
        outReason = Report::ReasonCode::kNone;
        return typed;
    }

    template <class T>
    [[nodiscard]] T* ResolveChecked(std::string_view key) const {
        Report::ReasonCode ignored = Report::ReasonCode::kNone;
        return ResolveChecked<T>(key, ignored);
    }

    /// Apply the alias map to a key's plugin component. Returns the key
    /// unchanged when no alias applies.
    [[nodiscard]] std::string ApplyAliases(std::string_view key) const;

private:
    FormResolver() = default;

    // lowercase source plugin -> replacement plugin, as written by the user
    std::unordered_map<std::string, std::string> m_aliases;
    std::vector<std::string> m_missing;
};

}  // namespace SaveMigration::Model
