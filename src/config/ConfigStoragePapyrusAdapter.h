#pragma once

#include "config/ConfigStorage.h"
#include "log.h"

namespace Config {

/// Papyrus adapter - bridges SaveMigration_Config.psc to the ConfigStorage
/// singleton, so the MCM reads and writes exactly the keys the C++ reads and
/// writes. All functions take StaticFunctionTag* as first param (required for
/// global native functions).
///
/// The whole point of routing the menu through here rather than through MCM
/// Helper is that MCM Helper only ever reads its own
/// `Data/MCM/Settings/<mod>.ini`; it has no way to reach an INI a C++ plugin
/// owns. This adapter is the shared file.
///
/// Note: RegisterSelectOptions is NOT exposed to Papyrus - options are defined
/// in C++ only.
namespace PapyrusAdapter {
    using VM = RE::BSScript::IVirtualMachine;

    // ========== Int Functions ==========

    inline int GetIntValue(RE::StaticFunctionTag*, RE::BSFixedString optionName, int defaultValue)
    {
        return ConfigStorage::GetSingleton()->GetInt(optionName.c_str(), defaultValue);
    }

    inline int SetIntValue(RE::StaticFunctionTag*, RE::BSFixedString optionName, int value)
    {
        return ConfigStorage::GetSingleton()->SetInt(optionName.c_str(), value);
    }

    inline int ResetIntValue(RE::StaticFunctionTag*, RE::BSFixedString optionName, int defaultValue)
    {
        return ConfigStorage::GetSingleton()->ResetInt(optionName.c_str(), defaultValue);
    }

    // ========== Float Functions ==========

    inline float GetFloatValue(RE::StaticFunctionTag*, RE::BSFixedString optionName, float defaultValue)
    {
        return ConfigStorage::GetSingleton()->GetFloat(optionName.c_str(), defaultValue);
    }

    inline float SetFloatValue(RE::StaticFunctionTag*, RE::BSFixedString optionName, float value)
    {
        return ConfigStorage::GetSingleton()->SetFloat(optionName.c_str(), value);
    }

    inline float ResetFloatValue(RE::StaticFunctionTag*, RE::BSFixedString optionName, float defaultValue)
    {
        return ConfigStorage::GetSingleton()->ResetFloat(optionName.c_str(), defaultValue);
    }

    // ========== String Functions ==========

    inline RE::BSFixedString GetStringValue(RE::StaticFunctionTag*, RE::BSFixedString optionName,
                                             RE::BSFixedString defaultValue)
    {
        std::string result = ConfigStorage::GetSingleton()->GetString(
            optionName.c_str(), defaultValue.c_str());
        return RE::BSFixedString(result);
    }

    inline RE::BSFixedString SetStringValue(RE::StaticFunctionTag*, RE::BSFixedString optionName,
                                             RE::BSFixedString value)
    {
        std::string result = ConfigStorage::GetSingleton()->SetString(optionName.c_str(), value.c_str());
        return RE::BSFixedString(result);
    }

    inline RE::BSFixedString ResetStringValue(RE::StaticFunctionTag*, RE::BSFixedString optionName,
                                               RE::BSFixedString defaultValue)
    {
        std::string result = ConfigStorage::GetSingleton()->ResetString(
            optionName.c_str(), defaultValue.c_str());
        return RE::BSFixedString(result);
    }

    // ========== Form Functions ==========

    inline RE::TESForm* GetFormValue(RE::StaticFunctionTag*, RE::BSFixedString optionName,
                                      RE::TESForm* defaultValue)
    {
        return ConfigStorage::GetSingleton()->GetForm(optionName.c_str(), defaultValue);
    }

    inline RE::TESForm* SetFormValue(RE::StaticFunctionTag*, RE::BSFixedString optionName,
                                      RE::TESForm* value)
    {
        return ConfigStorage::GetSingleton()->SetForm(optionName.c_str(), value);
    }

    inline RE::TESForm* ResetFormValue(RE::StaticFunctionTag*, RE::BSFixedString optionName,
                                        RE::TESForm* defaultValue)
    {
        return ConfigStorage::GetSingleton()->ResetForm(optionName.c_str(), defaultValue);
    }

    // ========== Select Functions ==========
    // Note: RegisterSelectOptions is NOT exposed - options are defined in C++ only.

    inline RE::BSFixedString GetSelectValue(RE::StaticFunctionTag*, RE::BSFixedString optionName)
    {
        std::string result = ConfigStorage::GetSingleton()->GetSelect(optionName.c_str());
        return RE::BSFixedString(result);
    }

    inline RE::BSFixedString SetSelectValue(RE::StaticFunctionTag*, RE::BSFixedString optionName,
                                             RE::BSFixedString value)
    {
        std::string result = ConfigStorage::GetSingleton()->SetSelect(optionName.c_str(), value.c_str());
        return RE::BSFixedString(result);
    }

    inline RE::BSFixedString ResetSelectValue(RE::StaticFunctionTag*, RE::BSFixedString optionName)
    {
        std::string result = ConfigStorage::GetSingleton()->ResetSelect(optionName.c_str());
        return RE::BSFixedString(result);
    }

    /// Get registered options for a select key.
    /// Options are defined in C++ via ConfigStorage::RegisterSelectOptions().
    inline std::vector<RE::BSFixedString> GetSelectOptions(RE::StaticFunctionTag*,
                                                            RE::BSFixedString optionName)
    {
        auto options = ConfigStorage::GetSingleton()->GetSelectOptions(optionName.c_str());
        std::vector<RE::BSFixedString> result;
        result.reserve(options.size());
        for (const auto& opt : options) {
            result.emplace_back(opt);
        }
        return result;
    }

    // ========== Registration ==========

    inline bool Bind(VM* a_vm)
    {
        if (!a_vm) {
            spdlog::error("ConfigStoragePapyrusAdapter::Bind: VM is null");
            return false;
        }

        constexpr auto scriptName = "SaveMigration_Config"sv;

        // Int functions
        a_vm->RegisterFunction("GetIntValue"sv, scriptName, GetIntValue);
        a_vm->RegisterFunction("SetIntValue"sv, scriptName, SetIntValue);
        a_vm->RegisterFunction("ResetIntValue"sv, scriptName, ResetIntValue);

        // Float functions
        a_vm->RegisterFunction("GetFloatValue"sv, scriptName, GetFloatValue);
        a_vm->RegisterFunction("SetFloatValue"sv, scriptName, SetFloatValue);
        a_vm->RegisterFunction("ResetFloatValue"sv, scriptName, ResetFloatValue);

        // String functions
        a_vm->RegisterFunction("GetStringValue"sv, scriptName, GetStringValue);
        a_vm->RegisterFunction("SetStringValue"sv, scriptName, SetStringValue);
        a_vm->RegisterFunction("ResetStringValue"sv, scriptName, ResetStringValue);

        // Form functions
        a_vm->RegisterFunction("GetFormValue"sv, scriptName, GetFormValue);
        a_vm->RegisterFunction("SetFormValue"sv, scriptName, SetFormValue);
        a_vm->RegisterFunction("ResetFormValue"sv, scriptName, ResetFormValue);

        // Select functions (read-only from Papyrus - options defined in C++)
        a_vm->RegisterFunction("GetSelectValue"sv, scriptName, GetSelectValue);
        a_vm->RegisterFunction("SetSelectValue"sv, scriptName, SetSelectValue);
        a_vm->RegisterFunction("ResetSelectValue"sv, scriptName, ResetSelectValue);
        a_vm->RegisterFunction("GetSelectOptions"sv, scriptName, GetSelectOptions);

        spdlog::info("ConfigStoragePapyrusAdapter: Registered native functions for '{}'", scriptName);
        return true;
    }

} // namespace PapyrusAdapter
} // namespace Config
