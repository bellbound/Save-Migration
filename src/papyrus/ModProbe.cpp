#include "papyrus/ModProbe.h"

#include <algorithm>
#include <cctype>

#include <Windows.h>

#include "core/Category.h"
#include "util/StringUtil.h"

namespace SaveMigration::Papyrus {

namespace {

std::string LowerCopy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

constexpr std::string_view kKnownScripts[] = {
    Known::kMhiyhQuestScript,     Known::kNffHomeScript,       Known::kNffControllerScript,
    Known::kFertilityStorageScript, Known::kDudestiaSubjectScript, Known::kDudestiaMainScript,
    Known::kObodyNativeScript,    Known::kTngScript,           Known::kStorageUtilScript,
    Known::kDialogueFollowerScript,
};

constexpr std::string_view kKnownDlls[] = {
    Known::kDressUpDll, Known::kSkyrimNetDll, Known::kObodyDll, Known::kTngDll,
    Known::kPapyrusUtilDll,
};

}  // namespace

ModProbe& ModProbe::Get() {
    static ModProbe instance;
    return instance;
}

void ModProbe::ProbeDlls() {
    if (m_dllsProbed) {
        return;
    }
    m_dllsProbed = true;

    for (const auto name : kKnownDlls) {
        const std::string dll(name);
        // GetModuleHandleA does not bump the refcount, which is what we want -
        // we are only asking "is this loaded", not taking a reference.
        const bool present = GetModuleHandleA(dll.c_str()) != nullptr;
        m_dlls[LowerCopy(dll)] = present;
        spdlog::info("ModProbe: DLL {} {}", dll, present ? "found" : "not found");
    }
}

void ModProbe::Resolve() {
    if (m_resolved) {
        return;
    }

    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        spdlog::error("ModProbe: TESDataHandler unavailable - all plugin requirements will fail");
        m_resolved = true;
        return;
    }

    // One walk of the file list, cached. Every category's Requirement is
    // answered from this map afterwards.
    m_pluginOrder.clear();
    for (const auto* file : handler->files) {
        if (!file) {
            continue;
        }
        std::string name(file->GetFilename());
        m_plugins[LowerCopy(name)] = true;
        m_pluginOrder.push_back(std::move(name));
    }
    spdlog::info("ModProbe: {} plugins in load order", m_pluginOrder.size());

    ProbeKnownScripts();
    m_resolved = true;
}

void ModProbe::ProbeKnownScripts() {
    for (const auto name : kKnownScripts) {
        const std::string script(name);
        const bool present = ProbeScriptLive(script);
        m_scripts[LowerCopy(script)] = present;
        spdlog::info("ModProbe: script {} {}", script, present ? "found" : "not found");
    }
}

bool ModProbe::ProbeScriptLive(const std::string& scriptName) const {
    auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (!vm) {
        return false;
    }
    RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typeInfo;
    // GetScriptObjectType loads the .pex if it has not been touched yet, which
    // is exactly what we want at kDataLoaded: presence on disk is the question.
    return vm->GetScriptObjectType(RE::BSFixedString(scriptName.c_str()), typeInfo) &&
           static_cast<bool>(typeInfo);
}

bool ModProbe::HasPlugin(std::string_view pluginName) const {
    const auto it = m_plugins.find(LowerCopy(pluginName));
    return it != m_plugins.end() && it->second;
}

bool ModProbe::HasScript(std::string_view scriptName) const {
    const auto key = LowerCopy(scriptName);
    const auto it = m_scripts.find(key);
    if (it != m_scripts.end()) {
        return it->second;
    }
    // Not pre-probed: probe now and remember. Keeps the cache authoritative
    // without needing every caller's script name up front.
    const bool present = ProbeScriptLive(std::string(scriptName));
    m_scripts[key] = present;
    return present;
}

bool ModProbe::HasDll(std::string_view dllName) const {
    const auto it = m_dlls.find(LowerCopy(dllName));
    if (it != m_dlls.end()) {
        return it->second;
    }
    return GetModuleHandleA(std::string(dllName).c_str()) != nullptr;
}

bool ModProbe::IsSatisfied(const Core::Requirement& requirement) const {
    return FirstMissing(requirement).empty();
}

std::string ModProbe::FirstMissing(const Core::Requirement& requirement) const {
    for (const auto& plugin : requirement.plugins) {
        if (!HasPlugin(plugin)) {
            return std::format("plugin '{}'", plugin);
        }
    }
    for (const auto& script : requirement.scriptNames) {
        if (!HasScript(script)) {
            return std::format("script '{}'", script);
        }
    }
    for (const auto& dll : requirement.dllNames) {
        if (!HasDll(dll)) {
            return std::format("DLL '{}'", dll);
        }
    }
    return "";
}

}  // namespace SaveMigration::Papyrus
