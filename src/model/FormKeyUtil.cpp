#include "model/FormKeyUtil.h"

#include <format>

#include <charconv>

#include "util/StringUtil.h"

namespace SaveMigration::Model {

std::string FormKeyUtil::BuildFormKey(RE::TESObjectREFR* ref) {
    return BuildFormKey(static_cast<RE::TESForm*>(ref));
}

std::string FormKeyUtil::BuildFormKey(RE::TESForm* form) {
    if (!form) {
        return "";
    }

    auto* file = form->GetFile(0);
    if (!file) {
        spdlog::trace("FormKeyUtil: form {:08X} has no source file (dynamic)", form->GetFormID());
        return "";
    }
    return BuildFormKey(form->GetLocalFormID(), file->GetFilename());
}

std::string FormKeyUtil::BuildFormKey(RE::FormID localFormId, std::string_view pluginName) {
    return std::format("0x{:X}~{}", localFormId, pluginName);
}

std::optional<FormKeyUtil::ParsedKey> FormKeyUtil::ParseFormKey(std::string_view keyString) {
    if (keyString.size() < 4 || keyString.substr(0, 2) != "0x") {
        return std::nullopt;
    }

    const size_t tilde = keyString.find('~');
    if (tilde == std::string_view::npos || tilde <= 2) {
        return std::nullopt;
    }

    const auto hexPart = keyString.substr(2, tilde - 2);
    RE::FormID localFormId = 0;
    const auto result =
        std::from_chars(hexPart.data(), hexPart.data() + hexPart.size(), localFormId, 16);
    if (result.ec != std::errc{} || result.ptr != hexPart.data() + hexPart.size()) {
        return std::nullopt;
    }

    const auto pluginName = keyString.substr(tilde + 1);
    if (pluginName.empty()) {
        return std::nullopt;
    }
    return ParsedKey{localFormId, std::string(pluginName)};
}

const RE::TESFile* FormKeyUtil::FindFile(std::string_view pluginName) {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        return nullptr;
    }
    // LookupModByName is case-sensitive in some CommonLib versions; plugin
    // filenames on disk vary in case, so fall back to an explicit scan.
    if (const auto* file = handler->LookupModByName(pluginName)) {
        return file;
    }
    for (const auto* file : handler->files) {
        if (file && Util::IEquals(file->GetFilename(), pluginName)) {
            return file;
        }
    }
    return nullptr;
}

bool FormKeyUtil::IsPluginLoaded(std::string_view pluginName) {
    return FindFile(pluginName) != nullptr;
}

RE::TESForm* FormKeyUtil::Resolve(std::string_view keyString) {
    const auto parsed = ParseFormKey(keyString);
    if (!parsed) {
        spdlog::warn("FormKeyUtil: unparseable key '{}'", keyString);
        return nullptr;
    }

    // The `~DYNAMIC` sentinel the source project used is deliberately not
    // honoured. A dynamic FormID is an allocator value private to one save;
    // resolving it in a different save would hand back an unrelated object.
    if (Util::IEquals(parsed->pluginName, "DYNAMIC")) {
        spdlog::debug("FormKeyUtil: refusing to resolve dynamic key '{}'", keyString);
        return nullptr;
    }

    const auto* file = FindFile(parsed->pluginName);
    if (!file) {
        spdlog::debug("FormKeyUtil: plugin not loaded: {}", parsed->pluginName);
        return nullptr;
    }

    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        return nullptr;
    }
    // The one sanctioned resolution path: handles the VR/SE ESL split itself.
    return handler->LookupForm(parsed->localFormId, file->GetFilename());
}

}  // namespace SaveMigration::Model
