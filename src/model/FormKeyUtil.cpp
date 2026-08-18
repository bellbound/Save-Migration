#include "model/FormKeyUtil.h"

#include <format>

#include <charconv>

#include "util/StringUtil.h"

namespace SaveMigration::Model {

namespace {

/// The plugin a runtime FormID's own index bits name, and the local id left over
/// once those bits are stripped.
///
/// This is the exact inverse of `TESDataHandler::LookupFormID`, which is what
/// `Resolve` below goes through - deliberately, so a key built here always reads
/// back as the form it was built from.
///
/// It walks `handler->files`, the same list `LookupModByName` walks, rather than
/// `GetLoadedMods()`: on VR that accessor is a hard-coded struct offset, and
/// everything offset-dependent here is treated as suspect (`Core::VRLayoutProbe`).
///
/// **Whether ESL space exists is asked of the runtime, not of `IsVR()`.** Stock
/// Skyrim VR 1.4.15 has none, so an 0xFE top byte there is a plain compile index
/// of 254 - a real slot in a load order that big. But SkyrimVRESL gives VR a
/// genuine light-plugin collection, and this workspace runs it: the export
/// measured 2026-08-10 had 3055 plugins, 1915 of them light, carrying compile
/// index 254 with real small-file indices. Branching on `IsVR()` would be wrong
/// on one install or the other; `GetLoadedLightModCount()` answers the question
/// actually being asked, on both.
const RE::TESFile* OwningFile(RE::TESDataHandler* handler, RE::FormID formId,
                              RE::FormID& localId) {
    if ((formId & 0xFF000000u) == 0xFE000000u && handler->GetLoadedLightModCount() > 0) {
        const auto smallIndex = static_cast<uint16_t>((formId & 0x00FFF000u) >> 12);
        localId = formId & 0x00000FFFu;
        for (const auto* file : handler->files) {
            if (file && file->compileIndex == 0xFE && file->smallFileCompileIndex == smallIndex) {
                return file;
            }
        }
        return nullptr;
    }

    const auto index = static_cast<uint8_t>((formId & 0xFF000000u) >> 24);
    localId = formId & 0x00FFFFFFu;
    for (const auto* file : handler->files) {
        if (file && file->compileIndex == index) {
            return file;
        }
    }
    return nullptr;
}

}  // namespace

std::string FormKeyUtil::BuildFormKey(RE::TESObjectREFR* ref) {
    return BuildFormKey(static_cast<RE::TESForm*>(ref));
}

std::string FormKeyUtil::BuildFormKey(RE::TESForm* form) {
    if (!form) {
        return "";
    }

    const RE::FormID formId = form->GetFormID();

    // A dynamic form is an allocator value private to one save. It names nothing
    // in another one, so it gets no key at all.
    if ((formId & 0xFF000000u) == 0xFF000000u) {
        spdlog::trace("FormKeyUtil: form {:08X} is dynamic; no plugin owns it", formId);
        return "";
    }

    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        spdlog::error("FormKeyUtil: no data handler; cannot name the owner of {:08X}", formId);
        return "";
    }

    // Deliberately *not* `sourceFiles` / `GetFile(0)`. That array lists plugins
    // that touch the form, and for an overridden record it can hold only the
    // overriding one - the plugin that never declared this id.
    //
    // Measured 2026-08-10: Keeper Carcette, a vanilla NPC, was harvested with
    // refKey '0xC3B2B~3DNPC.esp'. Her reference is Skyrim.esm's 0x000C3B2B and
    // 3DNPC.esp merely overrides the cell it sits in, so that key asked the
    // import for 3DNPC's 0xC3B2B - a form nothing declares. The import reported
    // her as missing from a save she was standing in. Her *base* key came out
    // right, because base records keep an ordered source list; references do
    // not, which is why this only bit reference keys.
    //
    // The FormID's own index bits are not a hint, they are the answer: the
    // engine builds a runtime id by pasting the owning plugin's load order index
    // onto its local id, and that is what the in-game console reports. Reading
    // them back makes this function the exact inverse of `Resolve`.
    RE::FormID localId = 0;
    const auto* file = OwningFile(handler, formId, localId);
    if (!file) {
        spdlog::warn("FormKeyUtil: no loaded plugin holds index of form {:08X}", formId);
        return "";
    }

    // Read the key straight back through the function the restore uses. This is
    // the whole contract in one line, and it costs one lookup per harvested form.
    // It logs rather than discards: the load order is the authority on who owns
    // an id, so a disagreement here is a fact worth seeing, not grounds for
    // throwing the form away. Nothing like it existed before, which is why a
    // wrong key could be written for a year without anyone noticing.
    if (handler->LookupForm(localId, file->GetFilename()) != form) {
        spdlog::warn("FormKeyUtil: {:08X} keyed as 0x{:X}~{} does not read back as itself", formId,
                     localId, file->GetFilename());
    }

    return BuildFormKey(localId, file->GetFilename());
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
