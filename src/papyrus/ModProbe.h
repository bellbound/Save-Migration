#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SaveMigration::Core {
struct Requirement;
}

namespace SaveMigration::Papyrus {

/// One-shot detection of which third-party mods are actually present.
///
/// Three independent signals, because they fail independently:
/// - **Plugin present** but scripts missing: a partial install. The ESP loads,
///   quests exist, and every script dispatch silently no-ops.
/// - **Scripts present** but plugin missing: leftover loose files.
/// - **DLL present**: the only signal for a pure-SKSE mod like VR Dress Up.
///
/// Resolved once at kDataLoaded (DLLs earlier, at kPostLoad, because that is
/// when other plugins have loaded but not yet initialised) and cached for the
/// session. A script-type probe walks the VM's type table, which is not
/// something to repeat per category per actor.
class ModProbe {
public:
    static ModProbe& Get();

    /// Probe module handles. Call at kPostLoad / kPostPostLoad.
    void ProbeDlls();

    /// Probe plugins and script types. Call at kDataLoaded.
    void Resolve();

    [[nodiscard]] bool HasPlugin(std::string_view pluginName) const;
    [[nodiscard]] bool HasScript(std::string_view scriptName) const;
    [[nodiscard]] bool HasDll(std::string_view dllName) const;

    /// True when every element of the requirement is satisfied.
    [[nodiscard]] bool IsSatisfied(const Core::Requirement& requirement) const;

    /// First unsatisfied element, for the report line. Empty when satisfied.
    [[nodiscard]] std::string FirstMissing(const Core::Requirement& requirement) const;

    [[nodiscard]] bool IsResolved() const { return m_resolved; }

    /// Plugin filenames in load order, used by LoadOrderFingerprint.
    [[nodiscard]] const std::vector<std::string>& LoadedPlugins() const { return m_pluginOrder; }

private:
    ModProbe() = default;

    /// Script names this plugin cares about, probed eagerly so the VM walk
    /// happens once. Anything not on this list falls back to a live probe.
    void ProbeKnownScripts();
    bool ProbeScriptLive(const std::string& scriptName) const;

    bool m_resolved = false;
    bool m_dllsProbed = false;

    // lowercase key -> present
    std::unordered_map<std::string, bool> m_plugins;
    mutable std::unordered_map<std::string, bool> m_scripts;
    std::unordered_map<std::string, bool> m_dlls;
    std::vector<std::string> m_pluginOrder;
};

/// Names used by more than one category, kept in one place so a typo is a
/// compile error rather than a silent "mod not installed".
namespace Known {
// Plugins
constexpr std::string_view kMhiyhPlugin = "My Home Is Your Home.esp";
constexpr std::string_view kNffPlugin = "nwsFollowerFramework.esp";
constexpr std::string_view kFertilityPlugin = "Fertility Mode.esp";
constexpr std::string_view kDudestiaPlugin = "Dudestia's Outfit Changer.esp";
constexpr std::string_view kObodyPlugin = "OBody.esp";
constexpr std::string_view kTngPlugin = "TheNewGentleman.esp";
constexpr std::string_view kSkyrimNetPlugin = "SkyrimNet.esp";

// Script class names
constexpr std::string_view kMhiyhQuestScript = "vvvMarkHomeQuest";
constexpr std::string_view kNffHomeScript = "nwsFollowerHomeScript";
constexpr std::string_view kNffControllerScript = "nwsFollowerControllerScript";
constexpr std::string_view kFertilityStorageScript = "_JSW_BB_Storage";
constexpr std::string_view kDudestiaSubjectScript = "DudestiaOutfitChangerSubject";
constexpr std::string_view kDudestiaMainScript = "DudestiaOutfitChangerMain";
constexpr std::string_view kObodyNativeScript = "OBodyNative";
constexpr std::string_view kTngScript = "TNG_PapyrusUtil";
constexpr std::string_view kStorageUtilScript = "StorageUtil";
constexpr std::string_view kDialogueFollowerScript = "DialogueFollowerScript";

// DLLs
constexpr std::string_view kDressUpDll = "DressUpVR.dll";
constexpr std::string_view kSkyrimNetDll = "SkyrimNet.dll";
constexpr std::string_view kObodyDll = "OBody.dll";
constexpr std::string_view kTngDll = "TheNewGentleman.dll";
constexpr std::string_view kPapyrusUtilDll = "PapyrusUtil.dll";
}  // namespace Known

}  // namespace SaveMigration::Papyrus
