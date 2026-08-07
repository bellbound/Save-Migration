#pragma once

#include <RE/Skyrim.h>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace Config {

/// Thread-safe INI-backed configuration storage.
/// Usable directly from C++ or via Papyrus native functions (VRSexMenu_Config.psc).
///
/// INI File: Data\SKSE\Plugins\<ModName>\<ModName>_config.ini
/// Sections: Values are stored in [General] section, or custom sections via qualified names "Section:Key"
///
/// Select options are defined in C++ via RegisterSelectOptions() - Papyrus can only read them.
class ConfigStorage
{
public:
    static ConfigStorage* GetSingleton();

    /// Initialize with mod name. Must be called once at plugin load.
    /// Creates directory structure if needed.
    /// @param modName - Name used for folder and INI file (e.g., "VRSexMenu")
    void Initialize(std::string_view modName);

    bool IsInitialized() const { return m_initialized; }
    const std::string& GetModName() const { return m_modName; }
    const std::string& GetIniPath() const { return m_iniPath; }

    // ========== Registration (C++ only, writes default if key missing) ==========

    /// Register an int option with default value (C++ only).
    /// If key doesn't exist in INI, writes defaultValue.
    /// Returns current value (existing or newly written default).
    int RegisterIntOption(std::string_view key, int defaultValue);

    /// Register a float option with default value (C++ only).
    float RegisterFloatOption(std::string_view key, float defaultValue);

    /// Register a string option with default value (C++ only).
    std::string RegisterStringOption(std::string_view key, std::string_view defaultValue);

    // ========== Int (also used for booleans) ==========

    /// Get int value, returning defaultValue if not found.
    /// Does NOT write default to file - use RegisterIntOption or SetInt for that.
    int GetInt(std::string_view key, int defaultValue = 0);

    /// Set int value (creates INI file if needed).
    /// Returns the value that was set.
    int SetInt(std::string_view key, int value);

    /// Reset to default value (writes default to file).
    int ResetInt(std::string_view key, int defaultValue);

    // ========== Float ==========

    /// Get float value, returning defaultValue if not found.
    /// Does NOT write default to file - use RegisterFloatOption or SetFloat for that.
    float GetFloat(std::string_view key, float defaultValue = 0.0f);
    float SetFloat(std::string_view key, float value);
    float ResetFloat(std::string_view key, float defaultValue);

    // ========== String ==========

    /// Get string value, returning defaultValue if not found.
    /// Does NOT write default to file - use RegisterStringOption or SetString for that.
    std::string GetString(std::string_view key, std::string_view defaultValue = "");
    std::string SetString(std::string_view key, std::string_view value);
    std::string ResetString(std::string_view key, std::string_view defaultValue);

    // ========== Form (stored as FormKey strings) ==========

    /// Get form, resolving from stored FormKey string.
    /// Returns defaultValue if key not found or form can't be resolved.
    RE::TESForm* GetForm(std::string_view key, RE::TESForm* defaultValue = nullptr);

    /// Set form, storing as FormKey string (load-order independent).
    /// Returns the form that was set (same as input).
    RE::TESForm* SetForm(std::string_view key, RE::TESForm* value);

    RE::TESForm* ResetForm(std::string_view key, RE::TESForm* defaultValue);

    // ========== Select (dropdown/menu options) ==========
    // Options are defined in C++ only - Papyrus can read but not register.

    /// Register available options for a select key (C++ only).
    /// @param key - The option key (e.g., "Controls:sGrabMode")
    /// @param options - List of valid option strings
    /// @param defaultValue - Default selection (must be in options list, or empty for first option)
    /// Writes default to INI if key doesn't exist.
    void RegisterSelectOptions(std::string_view key, const std::vector<std::string>& options,
                               std::string_view defaultValue = "");

    /// Get currently selected option string.
    /// Returns first option if not set, or "Option Not Found" if options not registered.
    std::string GetSelect(std::string_view key);

    /// Set selected option (validates against registered options).
    /// Returns the option string that was set, or current value if invalid.
    std::string SetSelect(std::string_view key, std::string_view value);

    /// Get registered options for a key.
    /// Returns {"Option Not Found"} if not registered.
    std::vector<std::string> GetSelectOptions(std::string_view key);

    /// Reset to default (first registered option).
    std::string ResetSelect(std::string_view key);

    // ========== Utility ==========

    /// Force write in-memory cache to disk (normally auto-writes on each Set).
    void FlushToDisk();

    /// Reload all values from disk (discards in-memory changes).
    void ReloadFromDisk();

private:
    ConfigStorage() = default;
    ~ConfigStorage() = default;
    ConfigStorage(const ConfigStorage&) = delete;
    ConfigStorage& operator=(const ConfigStorage&) = delete;

    // Parse "Section:Key" format, returning {section, key}
    // If no ":" present, returns {"General", fullKey}
    std::pair<std::string, std::string> ParseSectionKey(std::string_view qualifiedKey);

    // Ensure INI file exists (creates empty file with header comment)
    void EnsureIniFileExists();

    // INI accessors, backed by SimpleIni rather than the Win32 profile API - see the
    // note at the top of ConfigStorage.cpp for why. SimpleIni stays out of this header
    // deliberately; the load helper is file-local to the .cpp.
    std::string ReadIniString(const std::string& section, const std::string& key,
                               const std::string& defaultValue);
    int ReadIniInt(const std::string& section, const std::string& key, int defaultValue);
    void WriteIniString(const std::string& section, const std::string& key,
                        const std::string& value);

    // Check if a key exists in the INI file
    bool KeyExists(const std::string& section, const std::string& key);

    bool m_initialized = false;
    std::string m_modName;
    std::string m_iniPath;       // Full path to INI file
    std::string m_iniFolderPath; // Folder containing INI file

    // Select options registry: key -> list of valid options (defined in C++)
    std::unordered_map<std::string, std::vector<std::string>> m_selectOptions;

    // Registered defaults (set by Register*Option, used by Reset*)
    std::unordered_map<std::string, int> m_intDefaults;
    std::unordered_map<std::string, float> m_floatDefaults;
    std::unordered_map<std::string, std::string> m_stringDefaults;
    std::unordered_map<std::string, std::string> m_selectDefaults;  // key -> default option string

    // Thread safety
    mutable std::mutex m_mutex;
};

} // namespace Config
