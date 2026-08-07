#include "model/FormRef.h"

#include <algorithm>
#include <cctype>

#include "util/StringUtil.h"

namespace SaveMigration::Model {

namespace {

std::string LowerCopy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string SafeName(RE::TESForm* form) {
    if (!form) {
        return "";
    }
    const char* name = form->GetName();
    if (name && *name) {
        return Util::ConvertSkyrimTextToUTF8(name);
    }
    if (const char* editorId = form->GetFormEditorID(); editorId && *editorId) {
        return Util::ConvertSkyrimTextToUTF8(editorId);
    }
    return std::format("[{:08X}]", form->GetFormID());
}

}  // namespace

FormRef FormRef::From(RE::TESForm* form) {
    FormRef ref;
    ref.key = FormKeyUtil::BuildFormKey(form);
    ref.displayName = SafeName(form);
    return ref;
}

FormRef FormRef::From(RE::TESObjectREFR* refr) {
    return From(static_cast<RE::TESForm*>(refr));
}

FormResolver& FormResolver::Get() {
    static FormResolver instance;
    return instance;
}

void FormResolver::SetAliases(std::string_view spec) {
    m_aliases.clear();
    for (const auto& pair : Util::SplitAndTrim(spec, ',')) {
        const size_t eq = pair.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= pair.size()) {
            spdlog::warn("FormResolver: ignoring malformed plugin alias '{}' (expected Old.esp=New.esp)",
                         pair);
            continue;
        }
        auto from = Util::Trim(std::string_view(pair).substr(0, eq));
        auto to = Util::Trim(std::string_view(pair).substr(eq + 1));
        if (from.empty() || to.empty()) {
            continue;
        }
        spdlog::info("FormResolver: plugin alias '{}' -> '{}'", from, to);
        m_aliases.emplace(LowerCopy(from), std::move(to));
    }
}

void FormResolver::SetMissingPlugins(std::vector<std::string> plugins) {
    m_missing = std::move(plugins);
}

bool FormResolver::IsPluginKnownMissing(std::string_view pluginName) const {
    return std::any_of(m_missing.begin(), m_missing.end(),
                       [&](const std::string& p) { return Util::IEquals(p, pluginName); });
}

std::string FormResolver::ApplyAliases(std::string_view key) const {
    if (m_aliases.empty()) {
        return std::string(key);
    }
    const auto parsed = FormKeyUtil::ParseFormKey(key);
    if (!parsed) {
        return std::string(key);
    }
    const auto it = m_aliases.find(LowerCopy(parsed->pluginName));
    if (it == m_aliases.end()) {
        return std::string(key);
    }
    return FormKeyUtil::BuildFormKey(parsed->localFormId, it->second);
}

ResolveResult FormResolver::Resolve(std::string_view key) const {
    if (key.empty()) {
        return {nullptr, Report::ReasonCode::kDynamicForm};
    }

    const auto aliased = ApplyAliases(key);
    const auto parsed = FormKeyUtil::ParseFormKey(aliased);
    if (!parsed) {
        return {nullptr, Report::ReasonCode::kFormLookupFailed};
    }

    // Pre-computed miss: answer without touching the data handler at all. This
    // is what keeps a disabled plugin from generating thousands of lookups.
    if (IsPluginKnownMissing(parsed->pluginName) ||
        !FormKeyUtil::IsPluginLoaded(parsed->pluginName)) {
        return {nullptr, Report::ReasonCode::kSourcePluginMissing};
    }

    auto* form = FormKeyUtil::Resolve(aliased);
    if (!form) {
        return {nullptr, Report::ReasonCode::kFormLookupFailed};
    }
    return {form, Report::ReasonCode::kNone};
}

}  // namespace SaveMigration::Model
