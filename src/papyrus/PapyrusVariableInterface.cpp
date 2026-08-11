#include "papyrus/PapyrusVariableInterface.h"

#include <algorithm>
#include <format>
#include <unordered_set>

#include "util/StringUtil.h"

namespace RE::BSScript {
/// CommonLib does not expose this in a header, but the VM's ForEachBoundObject
/// takes it. Declared here to match the vtable the VM calls.
class IForEachScriptObjectFunctor {
public:
    virtual ~IForEachScriptObjectFunctor() = default;
    virtual bool Visit(Object* script, void* arg2) = 0;
};
}  // namespace RE::BSScript

namespace SaveMigration::Papyrus {

namespace {

std::string LowerCopy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Papyrus decorates member variables as `::Name_var`. Both spellings reach the
/// same slot, so accept either.
std::string CleanName(const std::string& name) {
    std::string clean = name;
    if (clean.size() > 4 && clean.compare(clean.size() - 4, 4, "_var") == 0) {
        clean.erase(clean.size() - 4);
    }
    if (clean.size() > 2 && clean.compare(0, 2, "::") == 0) {
        clean.erase(0, 2);
    }
    return clean;
}

class ScriptNameCollector final : public RE::BSScript::IForEachScriptObjectFunctor {
public:
    std::vector<std::string> names;

    bool Visit(RE::BSScript::Object* script, void*) override {
        if (script && script->type) {
            if (const char* name = script->type->name.c_str(); name && *name) {
                names.emplace_back(name);
            }
        }
        return true;
    }
};

}  // namespace

PapyrusVariableInterface* PapyrusVariableInterface::GetSingleton() {
    static PapyrusVariableInterface instance;
    return &instance;
}

RE::BSScript::Internal::VirtualMachine* PapyrusVariableInterface::GetVM() {
    return RE::BSScript::Internal::VirtualMachine::GetSingleton();
}

// ═══════════════════════════════════════════════════════════════════════════
// Quest lookup
// ═══════════════════════════════════════════════════════════════════════════

RE::TESQuest* PapyrusVariableInterface::FindQuestByEditorID(const std::string& editorId) {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        return nullptr;
    }
    for (auto* quest : handler->GetFormArray<RE::TESQuest>()) {
        if (!quest) {
            continue;
        }
        if (const char* id = quest->GetFormEditorID(); id && Util::IEquals(id, editorId)) {
            return quest;
        }
    }
    return nullptr;
}

RE::TESQuest* PapyrusVariableInterface::FindQuestByFormID(RE::FormID formId) {
    auto* form = RE::TESForm::LookupByID(formId);
    return form ? form->As<RE::TESQuest>() : nullptr;
}

RE::TESQuest* PapyrusVariableInterface::FindQuestByScriptName(const std::string& scriptName) {
    const auto key = LowerCopy(scriptName);
    if (const auto it = m_questByScript.find(key); it != m_questByScript.end()) {
        return it->second;
    }

    auto* vm = GetVM();
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!vm || !handler) {
        return nullptr;
    }
    auto* policy = vm->GetObjectHandlePolicy();
    if (!policy) {
        return nullptr;
    }

    // A full quest walk with a bound-script probe on each. Expensive, so the
    // result - including a negative - is cached for the session.
    for (auto* quest : handler->GetFormArray<RE::TESQuest>()) {
        if (!quest) {
            continue;
        }
        const auto handle = policy->GetHandleForObject(quest->GetFormType(), quest);
        if (handle == policy->EmptyHandle()) {
            continue;
        }
        RE::BSTSmartPointer<RE::BSScript::Object> object;
        if (vm->FindBoundObject(handle, scriptName.c_str(), object) && object) {
            spdlog::info("PapyrusVariableInterface: script '{}' is on quest {:08X}", scriptName,
                         quest->GetFormID());
            m_questByScript[key] = quest;
            return quest;
        }
    }

    spdlog::info("PapyrusVariableInterface: no quest carries script '{}'", scriptName);
    m_questByScript[key] = nullptr;
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
// Metadata
// ═══════════════════════════════════════════════════════════════════════════

std::vector<ScriptVariableInfo> PapyrusVariableInterface::GetScriptVariables(
    const std::string& scriptName) {
    std::vector<ScriptVariableInfo> result;
    auto* vm = GetVM();
    if (!vm) {
        return result;
    }

    RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typeInfo;
    if (!vm->GetScriptObjectType(RE::BSFixedString(scriptName.c_str()), typeInfo) || !typeInfo) {
        return result;
    }

    // Walk the type and then its bases. Inherited properties are as writable as
    // declared ones, and a mod that moves a property onto a shared base script
    // between releases would otherwise read as "the property is gone".
    for (auto* type = typeInfo.get(); type; type = type->GetParent()) {
        // Every GetXIter() computes an offset into `data`, which only holds the
        // table blob once the type is linked - before that it is an
        // UnlinkedNativeFunction list head. Reading the tables off an unlinked
        // type hands back arbitrary bytes as BSFixedString, and dereferencing
        // those is an access violation, not an empty result.
        if (!type->IsLinked()) {
            spdlog::warn("PapyrusVariableInterface: script type '{}' is not linked; its variables "
                         "cannot be enumerated, so every write against '{}' will be refused",
                         type->GetName() ? type->GetName() : scriptName.c_str(), scriptName);
            break;
        }

        const auto propertyCount = type->GetNumProperties();
        if (const auto* properties = type->GetPropertyIter()) {
            for (uint32_t i = 0; i < propertyCount; ++i) {
                if (const char* name = properties[i].name.c_str(); name && *name) {
                    result.push_back({name, properties[i].info.type.TypeAsString(), true});
                }
            }
        }
        // GetNumVariables(), *not* GetTotalNumVariables(): the total counts the
        // whole inheritance chain, but GetVariableIter() points at this type's own
        // array only - the entries past it are the initial-value and property
        // tables. The parents' variables are reached by the loop instead.
        const auto variableCount = type->GetNumVariables();
        if (const auto* variables = type->GetVariableIter()) {
            for (uint32_t i = 0; i < variableCount; ++i) {
                if (const char* name = variables[i].name.c_str(); name && *name) {
                    result.push_back({name, variables[i].type.TypeAsString(), false});
                }
            }
        }
    }
    return result;
}

bool PapyrusVariableInterface::AssertPropertyType(const std::string& scriptName,
                                                 const std::string& name,
                                                 const std::string& expectedType) {
    const auto clean = LowerCopy(CleanName(name));
    const auto wanted = LowerCopy(expectedType);

    for (const auto& info : GetScriptVariables(scriptName)) {
        if (LowerCopy(CleanName(info.name)) != clean) {
            continue;
        }
        const auto actual = LowerCopy(info.typeName);
        // Accept "Type" against "Type[]" in both directions: the caller says what
        // it means to write, and an array element write names the element type.
        if (actual == wanted || actual == wanted + "[]" || actual + "[]" == wanted) {
            return true;
        }
        spdlog::warn(
            "PapyrusVariableInterface: {}::{} is '{}' but a '{}' write was requested - refusing. "
            "This usually means the installed version of the mod differs from the one this build "
            "expects.",
            scriptName, name, info.typeName, expectedType);
        return false;
    }

    spdlog::warn("PapyrusVariableInterface: {}::{} does not exist on that script", scriptName, name);
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// Slot resolution
// ═══════════════════════════════════════════════════════════════════════════

bool PapyrusVariableInterface::ResolveQuestHandle(RE::TESQuest* quest, RE::VMHandle& handleOut) {
    auto* vm = GetVM();
    if (!vm || !quest) {
        return false;
    }
    auto* policy = vm->GetObjectHandlePolicy();
    if (!policy) {
        return false;
    }
    handleOut = policy->GetHandleForObject(quest->GetFormType(), quest);
    return handleOut != policy->EmptyHandle();
}

bool PapyrusVariableInterface::ResolveAliasHandle(RE::BGSBaseAlias* alias, RE::VMHandle& handleOut) {
    auto* vm = GetVM();
    if (!vm || !alias) {
        return false;
    }
    auto* policy = vm->GetObjectHandlePolicy();
    if (!policy) {
        return false;
    }
    // The raw overload: an alias is not a TESForm, and it carries its own VMTypeID.
    handleOut = policy->GetHandleForObject(alias->GetVMTypeID(), alias);
    return handleOut != policy->EmptyHandle();
}

RE::BSScript::Variable* PapyrusVariableInterface::FindSlot(RE::VMHandle handle,
                                                          const std::string& scriptName,
                                                          const std::string& variableName) {
    auto* vm = GetVM();
    if (!vm) {
        return nullptr;
    }
    RE::BSTSmartPointer<RE::BSScript::Object> object;
    if (!vm->FindBoundObject(handle, scriptName.c_str(), object) || !object) {
        return nullptr;
    }
    const auto clean = CleanName(variableName);
    // Both accessors hand back a *writable* pointer, which is what makes the write
    // half possible at all.
    if (auto* slot = object->GetProperty(clean.c_str())) {
        return slot;
    }
    return object->GetVariable(clean.c_str());
}

// ═══════════════════════════════════════════════════════════════════════════
// Conversion
// ═══════════════════════════════════════════════════════════════════════════

std::string PapyrusVariableInterface::GetVariableTypeName(RE::BSScript::Variable& var) {
    if (var.IsNoneObject() || var.IsNoneArray()) return "None";
    if (var.IsInt()) return "Int";
    if (var.IsFloat()) return "Float";
    if (var.IsBool()) return "Bool";
    if (var.IsString()) return "String";
    if (var.IsArray()) return "Array";
    if (var.IsObject()) {
        if (auto object = var.GetObject(); object && object->type) {
            return object->type->name.c_str();
        }
        return "Object";
    }
    return "Unknown";
}

VariableValue PapyrusVariableInterface::ConvertVariable(RE::BSScript::Variable& var) {
    if (var.IsNoneObject() || var.IsNoneArray()) {
        return std::monostate{};
    }
    if (var.IsInt()) return var.GetSInt();
    if (var.IsFloat()) return var.GetFloat();
    if (var.IsBool()) return var.GetBool();
    if (var.IsString()) {
        const auto sv = var.GetString();
        return std::string(sv.data(), sv.size());
    }
    if (var.IsObject()) {
        // The source project resolved the handle with
        // `GetObjectForHandle(RE::FormType::None, handle)`. FormType::None is not a
        // valid VMTypeID, so that call returned nullptr for most forms and every
        // object variable read back as FormID 0. `Unpack` consults the type table
        // and works for any form type.
        if (auto* form = var.Unpack<RE::TESForm*>()) {
            return form->GetFormID();
        }
        return RE::FormID{0};
    }
    return std::monostate{};
}

// ═══════════════════════════════════════════════════════════════════════════
// Reads
// ═══════════════════════════════════════════════════════════════════════════

VariableReadResult PapyrusVariableInterface::GetVariable(RE::TESQuest* quest,
                                                        const std::string& scriptName,
                                                        const std::string& variableName) {
    VariableReadResult result;
    RE::VMHandle handle{};
    if (!ResolveQuestHandle(quest, handle)) {
        result.error = "no VM handle for the quest";
        return result;
    }
    auto* slot = FindSlot(handle, scriptName, variableName);
    if (!slot) {
        result.error = std::format("'{}' not found on script '{}'", variableName, scriptName);
        return result;
    }
    result.success = true;
    result.typeName = GetVariableTypeName(*slot);
    result.value = ConvertVariable(*slot);
    return result;
}

VariableReadResult PapyrusVariableInterface::GetVariable(const std::string& questEditorId,
                                                        const std::string& scriptName,
                                                        const std::string& variableName) {
    auto* quest = FindQuestByEditorID(questEditorId);
    if (!quest) {
        return {false, std::format("quest '{}' not found", questEditorId), {}, ""};
    }
    return GetVariable(quest, scriptName, variableName);
}

ArrayReadResult PapyrusVariableInterface::GetArrayVariable(RE::TESQuest* quest,
                                                          const std::string& scriptName,
                                                          const std::string& variableName) {
    ArrayReadResult result;
    RE::VMHandle handle{};
    if (!ResolveQuestHandle(quest, handle)) {
        result.error = "no VM handle for the quest";
        return result;
    }
    auto* slot = FindSlot(handle, scriptName, variableName);
    if (!slot) {
        result.error = std::format("'{}' not found on script '{}'", variableName, scriptName);
        return result;
    }
    if (!slot->IsArray()) {
        result.error = std::format("'{}' is not an array", variableName);
        return result;
    }
    auto array = slot->GetArray();
    if (!array) {
        result.error = "array data unavailable";
        return result;
    }
    result.success = true;
    result.length = static_cast<int32_t>(array->size());
    result.elements.reserve(array->size());
    for (uint32_t i = 0; i < array->size(); ++i) {
        auto& element = (*array)[i];
        if (i == 0) {
            result.elementTypeName = GetVariableTypeName(element);
        }
        result.elements.push_back(ConvertVariable(element));
    }
    return result;
}

std::optional<int32_t> PapyrusVariableInterface::GetInt(RE::TESQuest* quest,
                                                       const std::string& scriptName,
                                                       const std::string& name) {
    const auto result = GetVariable(quest, scriptName, name);
    if (!result.success) return std::nullopt;
    if (const auto* value = std::get_if<int32_t>(&result.value)) return *value;
    return std::nullopt;
}

std::optional<float> PapyrusVariableInterface::GetFloat(RE::TESQuest* quest,
                                                        const std::string& scriptName,
                                                        const std::string& name) {
    const auto result = GetVariable(quest, scriptName, name);
    if (!result.success) return std::nullopt;
    if (const auto* value = std::get_if<float>(&result.value)) return *value;
    // A Papyrus Int in a Float-typed slot is common in hand-written scripts.
    if (const auto* value = std::get_if<int32_t>(&result.value)) return static_cast<float>(*value);
    return std::nullopt;
}

std::optional<bool> PapyrusVariableInterface::GetBool(RE::TESQuest* quest,
                                                     const std::string& scriptName,
                                                     const std::string& name) {
    const auto result = GetVariable(quest, scriptName, name);
    if (!result.success) return std::nullopt;
    if (const auto* value = std::get_if<bool>(&result.value)) return *value;
    return std::nullopt;
}

std::optional<std::string> PapyrusVariableInterface::GetString(RE::TESQuest* quest,
                                                              const std::string& scriptName,
                                                              const std::string& name) {
    const auto result = GetVariable(quest, scriptName, name);
    if (!result.success) return std::nullopt;
    if (const auto* value = std::get_if<std::string>(&result.value)) return *value;
    return std::nullopt;
}

std::optional<RE::FormID> PapyrusVariableInterface::GetFormID(RE::TESQuest* quest,
                                                             const std::string& scriptName,
                                                             const std::string& name) {
    const auto result = GetVariable(quest, scriptName, name);
    if (!result.success) return std::nullopt;
    if (const auto* value = std::get_if<RE::FormID>(&result.value)) return *value;
    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════════════════
// Writes
// ═══════════════════════════════════════════════════════════════════════════

bool PapyrusVariableInterface::WriteSlot(RE::BSScript::Variable* slot, const VariableValue& value) {
    if (!slot) {
        return false;
    }
    return std::visit(
        [slot](auto&& arg) -> bool {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                slot->SetNone();
                return true;
            } else if constexpr (std::is_same_v<T, int32_t>) {
                slot->SetSInt(arg);
                return true;
            } else if constexpr (std::is_same_v<T, float>) {
                slot->SetFloat(arg);
                return true;
            } else if constexpr (std::is_same_v<T, bool>) {
                slot->SetBool(arg);
                return true;
            } else if constexpr (std::is_same_v<T, std::string>) {
                slot->SetString(arg);
                return true;
            } else if constexpr (std::is_same_v<T, RE::FormID>) {
                auto* form = RE::TESForm::LookupByID(arg);
                if (!form) {
                    slot->SetNone();
                    return arg == 0;  // writing 0 as None is intentional
                }
                slot->Pack(form);
                return true;
            }
            return false;
        },
        value);
}

bool PapyrusVariableInterface::SetVariable(RE::TESQuest* quest, const std::string& scriptName,
                                          const std::string& name, const VariableValue& value,
                                          const std::string& expectedType) {
    if (!AssertPropertyType(scriptName, name, expectedType)) {
        return false;
    }
    RE::VMHandle handle{};
    if (!ResolveQuestHandle(quest, handle)) {
        return false;
    }
    auto* slot = FindSlot(handle, scriptName, name);
    if (!slot) {
        spdlog::warn("PapyrusVariableInterface: no slot {}::{} to write", scriptName, name);
        return false;
    }
    return WriteSlot(slot, value);
}

std::optional<uint32_t> PapyrusVariableInterface::GetArrayLength(RE::TESQuest* quest,
                                                                const std::string& scriptName,
                                                                const std::string& name) {
    RE::VMHandle handle{};
    if (!ResolveQuestHandle(quest, handle)) {
        return std::nullopt;
    }
    auto* slot = FindSlot(handle, scriptName, name);
    if (!slot || !slot->IsArray()) {
        return std::nullopt;
    }
    auto array = slot->GetArray();
    return array ? std::optional<uint32_t>(array->size()) : std::nullopt;
}

bool PapyrusVariableInterface::SetArrayElement(RE::TESQuest* quest, const std::string& scriptName,
                                              const std::string& name, uint32_t index,
                                              const VariableValue& value) {
    RE::VMHandle handle{};
    if (!ResolveQuestHandle(quest, handle)) {
        return false;
    }
    auto* slot = FindSlot(handle, scriptName, name);
    if (!slot || !slot->IsArray()) {
        spdlog::warn("PapyrusVariableInterface: {}::{} is not a writable array", scriptName, name);
        return false;
    }
    auto array = slot->GetArray();
    if (!array) {
        return false;
    }
    if (index >= array->size()) {
        // Growth is impossible from here: BSScript::Array has no resize() and
        // MAX_SIZE is 128. Refusing loudly beats writing past the end.
        spdlog::warn(
            "PapyrusVariableInterface: refusing to write {}::{}[{}] - the array is only {} long, and "
            "arrays cannot be grown from native code (no resize, and MAX_SIZE is 128). The owning "
            "mod has to extend it first.",
            scriptName, name, index, array->size());
        return false;
    }
    return WriteSlot(&(*array)[index], value);
}

bool PapyrusVariableInterface::ReplaceArray(RE::TESQuest* quest, const std::string& scriptName,
                                           const std::string& name,
                                           const std::vector<VariableValue>& values) {
    RE::VMHandle handle{};
    if (!ResolveQuestHandle(quest, handle)) {
        return false;
    }
    auto* slot = FindSlot(handle, scriptName, name);
    if (!slot || !slot->IsArray()) {
        return false;
    }
    auto array = slot->GetArray();
    if (!array) {
        return false;
    }
    if (array->size() != values.size()) {
        spdlog::warn(
            "PapyrusVariableInterface: refusing to replace {}::{} - it is {} long but {} values were "
            "supplied, and the array cannot be resized from here.",
            scriptName, name, array->size(), values.size());
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < values.size(); ++i) {
        ok = WriteSlot(&(*array)[static_cast<uint32_t>(i)], values[i]) && ok;
    }
    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════
// Alias scope
// ═══════════════════════════════════════════════════════════════════════════

VariableReadResult PapyrusVariableInterface::GetAliasVariable(RE::BGSBaseAlias* alias,
                                                             const std::string& scriptName,
                                                             const std::string& variableName) {
    VariableReadResult result;
    RE::VMHandle handle{};
    if (!ResolveAliasHandle(alias, handle)) {
        result.error = "no VM handle for the alias";
        return result;
    }
    auto* slot = FindSlot(handle, scriptName, variableName);
    if (!slot) {
        result.error = std::format("'{}' not found on alias script '{}'", variableName, scriptName);
        return result;
    }
    result.success = true;
    result.typeName = GetVariableTypeName(*slot);
    result.value = ConvertVariable(*slot);
    return result;
}

bool PapyrusVariableInterface::SetAliasVariable(RE::BGSBaseAlias* alias,
                                               const std::string& scriptName,
                                               const std::string& name, const VariableValue& value,
                                               const std::string& expectedType) {
    if (!AssertPropertyType(scriptName, name, expectedType)) {
        return false;
    }
    RE::VMHandle handle{};
    if (!ResolveAliasHandle(alias, handle)) {
        return false;
    }
    auto* slot = FindSlot(handle, scriptName, name);
    if (!slot) {
        spdlog::warn("PapyrusVariableInterface: no alias slot {}::{} to write", scriptName, name);
        return false;
    }
    return WriteSlot(slot, value);
}

ArrayReadResult PapyrusVariableInterface::GetAliasArrayVariable(RE::BGSBaseAlias* alias,
                                                               const std::string& scriptName,
                                                               const std::string& name) {
    ArrayReadResult result;
    RE::VMHandle handle{};
    if (!ResolveAliasHandle(alias, handle)) {
        result.error = "no VM handle for the alias";
        return result;
    }
    auto* slot = FindSlot(handle, scriptName, name);
    if (!slot || !slot->IsArray()) {
        result.error = std::format("'{}' is not an array on the alias script", name);
        return result;
    }
    auto array = slot->GetArray();
    if (!array) {
        result.error = "array data unavailable";
        return result;
    }
    result.success = true;
    result.length = static_cast<int32_t>(array->size());
    for (uint32_t i = 0; i < array->size(); ++i) {
        auto& element = (*array)[i];
        if (i == 0) {
            result.elementTypeName = GetVariableTypeName(element);
        }
        result.elements.push_back(ConvertVariable(element));
    }
    return result;
}

std::vector<PapyrusVariableInterface::AliasEntry> PapyrusVariableInterface::EnumerateRefAliases(
    RE::TESQuest* quest) {
    std::vector<AliasEntry> entries;
    if (!quest) {
        return entries;
    }

    // The engine mutates this array as aliases fill, so take the documented read
    // lock rather than racing it.
    const RE::BSReadLockGuard guard(quest->aliasAccessLock);
    uint32_t index = 0;
    for (auto* base : quest->aliases) {
        if (!base) {
            ++index;
            continue;
        }
        if (base->GetVMTypeID() != RE::BGSRefAlias::VMTYPEID) {
            ++index;
            continue;
        }
        auto* refAlias = static_cast<RE::BGSRefAlias*>(base);
        AliasEntry entry;
        entry.alias = refAlias;
        entry.actor = refAlias->GetActorReference();
        entry.index = index++;
        entry.aliasId = base->aliasID;
        entry.name = base->aliasName.c_str() ? base->aliasName.c_str() : "";
        // kLoadedOnly matters: a fill against an unloaded actor silently fails, so
        // those subjects have to go on the deferred queue instead.
        entry.loadedOnly = base->flags.all(RE::BGSBaseAlias::FLAGS::kLoadedOnly);
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::optional<uint32_t> PapyrusVariableInterface::FindAliasIndexHolding(RE::TESQuest* quest,
                                                                       RE::Actor* actor) {
    if (!actor) {
        return std::nullopt;
    }
    const auto entries = EnumerateRefAliases(quest);
    for (uint32_t i = 0; i < entries.size(); ++i) {
        if (entries[i].actor == actor) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace SaveMigration::Papyrus
