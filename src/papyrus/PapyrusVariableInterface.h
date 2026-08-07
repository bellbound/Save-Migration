#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace SaveMigration::Papyrus {

/// A Papyrus variable's value, as far as we care about it.
using VariableValue = std::variant<std::monostate, int32_t, float, bool, std::string, RE::FormID>;

struct ScriptVariableInfo {
    std::string name;
    std::string typeName;  // "Int", "Float", "Bool", "String", "Actor", …
    bool isProperty = false;
};

struct VariableReadResult {
    bool success = false;
    std::string error;
    VariableValue value;
    std::string typeName;
};

struct ArrayReadResult {
    bool success = false;
    std::string error;
    std::vector<VariableValue> elements;
    std::string elementTypeName;
    int32_t length = 0;
};

/// Reading and writing another mod's Papyrus script state directly.
///
/// Direct memory access into the VM's variable storage, not a script dispatch, so
/// it is synchronous and cheap - but game thread only, and every write is gated on
/// a type assertion first. Writing a Float into an Int slot does not fail, it
/// silently reinterprets the bits.
class PapyrusVariableInterface {
public:
    static PapyrusVariableInterface* GetSingleton();

    // ── Quest lookup ──────────────────────────────────────────────────────
    RE::TESQuest* FindQuestByEditorID(const std::string& editorId);
    RE::TESQuest* FindQuestByFormID(RE::FormID formId);

    /// Find the quest that has `scriptName` attached.
    ///
    /// Needed because several mods' quests have no documented editor ID -
    /// Fertility Mode's `_JSW_BB_Storage` and NFF's `nwsFollowerHomeScript` among
    /// them - and the script name is the only stable handle on them. Results are
    /// cached, because this walks every quest and probes each for bound scripts.
    RE::TESQuest* FindQuestByScriptName(const std::string& scriptName);

    // ── Metadata ──────────────────────────────────────────────────────────
    std::vector<ScriptVariableInfo> GetScriptVariables(const std::string& scriptName);

    /// Gate for every write. Returns true when `name` on `scriptName` really is
    /// `expectedType` (case-insensitive, and accepting `Type[]` for arrays).
    bool AssertPropertyType(const std::string& scriptName, const std::string& name,
                            const std::string& expectedType);

    // ── Reads: quest scope ────────────────────────────────────────────────
    VariableReadResult GetVariable(RE::TESQuest* quest, const std::string& scriptName,
                                   const std::string& variableName);
    VariableReadResult GetVariable(const std::string& questEditorId, const std::string& scriptName,
                                   const std::string& variableName);
    ArrayReadResult GetArrayVariable(RE::TESQuest* quest, const std::string& scriptName,
                                     const std::string& variableName);

    std::optional<int32_t> GetInt(RE::TESQuest* quest, const std::string& scriptName,
                                  const std::string& name);
    std::optional<float> GetFloat(RE::TESQuest* quest, const std::string& scriptName,
                                  const std::string& name);
    std::optional<bool> GetBool(RE::TESQuest* quest, const std::string& scriptName,
                                const std::string& name);
    std::optional<std::string> GetString(RE::TESQuest* quest, const std::string& scriptName,
                                         const std::string& name);
    std::optional<RE::FormID> GetFormID(RE::TESQuest* quest, const std::string& scriptName,
                                        const std::string& name);

    // ── Writes: quest scope ───────────────────────────────────────────────
    // The read path already proves the mechanism: GetProperty/GetVariable hand back
    // a *writable* `BSScript::Variable*`. These add the write half, each gated on
    // AssertPropertyType.

    bool SetVariable(RE::TESQuest* quest, const std::string& scriptName, const std::string& name,
                     const VariableValue& value, const std::string& expectedType);

    /// Write one element of an existing array.
    ///
    /// **Element writes only - growth is refused.** `BSScript::Array` exposes no
    /// `resize()` and its `MAX_SIZE` is 128, so there is no supported way to extend
    /// one from here. A category that needs a longer array must ask the owning mod
    /// to grow it (Fertility Mode's `UpdateStorage()` does exactly this) and then
    /// write into the result.
    bool SetArrayElement(RE::TESQuest* quest, const std::string& scriptName,
                         const std::string& name, uint32_t index, const VariableValue& value);

    /// Overwrite every element of an array whose length already matches.
    /// Refuses when the sizes differ, for the same reason as above.
    bool ReplaceArray(RE::TESQuest* quest, const std::string& scriptName, const std::string& name,
                      const std::vector<VariableValue>& values);

    /// Length of an array variable, or nullopt if it is not an array.
    std::optional<uint32_t> GetArrayLength(RE::TESQuest* quest, const std::string& scriptName,
                                           const std::string& name);

    // ── Alias scope ───────────────────────────────────────────────────────
    // Dudestia's entire per-subject state lives on a ReferenceAlias-derived script,
    // not on a quest, so quest-scoped access cannot reach it at all.

    VariableReadResult GetAliasVariable(RE::BGSBaseAlias* alias, const std::string& scriptName,
                                        const std::string& variableName);
    bool SetAliasVariable(RE::BGSBaseAlias* alias, const std::string& scriptName,
                          const std::string& name, const VariableValue& value,
                          const std::string& expectedType);
    ArrayReadResult GetAliasArrayVariable(RE::BGSBaseAlias* alias, const std::string& scriptName,
                                          const std::string& name);

    /// Walk `quest->aliases` under `aliasAccessLock` and return the reference
    /// aliases together with the actor each currently holds.
    struct AliasEntry {
        RE::BGSRefAlias* alias = nullptr;
        RE::Actor* actor = nullptr;
        uint32_t index = 0;   // position in quest->aliases
        uint32_t aliasId = 0; // the alias's own ALST id
        std::string name;
        bool loadedOnly = false;  // kLoadedOnly: fills silently fail when unloaded
    };
    std::vector<AliasEntry> EnumerateRefAliases(RE::TESQuest* quest);

    /// Index in `EnumerateRefAliases` order that currently holds `actor`, or nullopt.
    ///
    /// This is the read-back that MHIYH restore depends on: `ForceAlias` always
    /// fills the *first empty* alias, so the index an actor lands in is not the
    /// index we asked for.
    std::optional<uint32_t> FindAliasIndexHolding(RE::TESQuest* quest, RE::Actor* actor);

private:
    PapyrusVariableInterface() = default;

    RE::BSScript::Internal::VirtualMachine* GetVM();
    RE::BSScript::Variable* FindSlot(RE::VMHandle handle, const std::string& scriptName,
                                     const std::string& variableName);
    bool ResolveQuestHandle(RE::TESQuest* quest, RE::VMHandle& handleOut);
    bool ResolveAliasHandle(RE::BGSBaseAlias* alias, RE::VMHandle& handleOut);
    bool WriteSlot(RE::BSScript::Variable* slot, const VariableValue& value);

    VariableValue ConvertVariable(RE::BSScript::Variable& var);
    std::string GetVariableTypeName(RE::BSScript::Variable& var);

    /// scriptName (lowercased) -> quest, for FindQuestByScriptName.
    std::unordered_map<std::string, RE::TESQuest*> m_questByScript;
};

}  // namespace SaveMigration::Papyrus
