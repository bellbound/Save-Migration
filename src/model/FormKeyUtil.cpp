#include "model/FormKeyUtil.h"

#include <format>

#include <charconv>

#include "util/StringUtil.h"

namespace SaveMigration::Model {

namespace {

/// The local FormID this form would carry if `file` were the plugin that
/// *defines* it. Mirrors `TESForm::GetLocalFormID`, but against a chosen file
/// rather than whichever one happens to sit at `sourceFiles[0]`.
RE::FormID LocalIdAgainst(const RE::TESForm* form, const RE::TESFile* file) {
    RE::FormID indexBits = static_cast<RE::FormID>(file->compileIndex) << (3 * 8);
    indexBits += static_cast<RE::FormID>(file->smallFileCompileIndex) << ((1 * 8) + 4);
    return form->GetFormID() & ~indexBits;
}

}  // namespace

std::string FormKeyUtil::BuildFormKey(RE::TESObjectREFR* ref) {
    return BuildFormKey(static_cast<RE::TESForm*>(ref));
}

std::string FormKeyUtil::BuildFormKey(RE::TESForm* form) {
    if (!form) {
        return "";
    }

    const auto* array = form->sourceFiles.array;
    if (!array || array->empty()) {
        spdlog::trace("FormKeyUtil: form {:08X} has no source file (dynamic)", form->GetFormID());
        return "";
    }

    // `sourceFiles` lists every plugin that touches this form, and the naive
    // `GetFile(0)` is the *winning override*, not the plugin that defines it.
    // For anything a patch touches those differ, and the resulting key is not
    // merely mislabelled - it is incoherent. `GetLocalFormID` strips the
    // defining plugin's index, so pairing that number with an override's name
    // asks for a form the override never declared.
    //
    // Measured 2026-08-09: a vanilla exterior cell came out of a harvest as
    // '0x93B6~Better Dynamic Snow SE.esp'. That plugin holds 85 CELL records
    // and every one of them is an override - it defines no cells at all - so
    // the key could only ever have resolved to nothing, or to something else.
    //
    // So each candidate is *tried* rather than assumed, through the same
    // LookupForm the restore will use. A candidate that hands the form back is
    // correct by construction, which is what makes this safe on VR: the
    // ESL-versus-full-index question stays inside CommonLib, where 1.4.15 has
    // no ESL space at all. Ties go to the earliest-loading file, so a form
    // defined by Skyrim.esm and patched by five mods is recorded against
    // Skyrim.esm.
    auto* handler = RE::TESDataHandler::GetSingleton();
    const RE::TESFile* best = nullptr;
    RE::FormID bestLocal = 0;

    for (const auto* file : *array) {
        if (!file) {
            continue;
        }
        const auto local = LocalIdAgainst(form, file);
        if (handler && handler->LookupForm(local, file->GetFilename()) != form) {
            continue;
        }
        if (!best || file->compileIndex < best->compileIndex ||
            (file->compileIndex == best->compileIndex &&
             file->smallFileCompileIndex < best->smallFileCompileIndex)) {
            best = file;
            bestLocal = local;
        }
    }

    if (!best) {
        // No candidate round-tripped. Fall back to the old behaviour rather than
        // dropping the form: a key that resolves to nothing is still better than
        // no key, because the report can name what was lost.
        const auto* file = form->GetFile(0);
        if (!file) {
            return "";
        }
        spdlog::debug("FormKeyUtil: no source file of {:08X} round-trips; recording it against '{}'",
                      form->GetFormID(), file->GetFilename());
        return BuildFormKey(form->GetLocalFormID(), file->GetFilename());
    }

    return BuildFormKey(bestLocal, best->GetFilename());
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
