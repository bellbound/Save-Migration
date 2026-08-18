#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace SaveMigration::Papyrus {

/// Values we can hand to a Papyrus function.
using PapyrusValue = std::variant<std::monostate,  // None
                                  int, float, bool, std::string, RE::TESForm*, RE::Actor*,
                                  RE::TESObjectREFR*, std::vector<RE::Actor*>,
                                  std::vector<std::string>>;

using IntCallback = std::function<void(int32_t)>;
using StringCallback = std::function<void(const std::string&)>;
using BoolCallback = std::function<void(bool)>;
using FormCallback = std::function<void(RE::TESForm*)>;
using StringArrayCallback = std::function<void(const std::vector<std::string>&)>;
using ActorArrayCallback = std::function<void(const std::vector<RE::Actor*>&)>;

/// Bridge to other mods' Papyrus surfaces.
///
/// **Everything here is asynchronous.** The source project's
/// `ResultCallbackFunctor::WaitForResult` is deliberately absent from this
/// version's used surface: called on the game thread it blocks waiting for the VM
/// that the game thread itself has to pump, so it deadlocks the VM it is waiting
/// on. Results arrive on the VM thread; a captor records them and, if a follow-up
/// engine call is needed, schedules an `AddTask`.
class PapyrusInterface {
public:
    static PapyrusInterface* GetSingleton();

    RE::BSScript::Internal::VirtualMachine* GetVM();

    /// How many calls this plugin has handed to the VM and had accepted, since
    /// the process started. Monotonic, thread-safe, never reset.
    ///
    /// Sampled either side of a step, it answers "did that step touch Papyrus?"
    /// without every category having to declare it - and a declaration is exactly
    /// what would go stale, since whether a category calls the VM depends on which
    /// mods are installed and on what the snapshot happens to contain. The import
    /// uses it to decide which steps have earned settle time: a step that put
    /// nothing into the VM has nothing to wait for.
    ///
    /// A dispatch is not a completion. This counts what the VM took, not what it
    /// finished, which is the whole reason waiting afterwards is necessary.
    [[nodiscard]] static std::uint64_t DispatchCount();

    /// True when `scriptName` declares a global function called `functionName`
    /// that can accept `argCount` arguments.
    ///
    /// Every CallGlobalFunction* checks this first: dispatching a function the
    /// script does not declare crashes inside the VM rather than failing cleanly.
    /// Answers are cached, so the type table is walked once per name+arity.
    bool HasGlobalFunction(const std::string& scriptName, const std::string& functionName,
                           std::size_t argCount);

    /// The method equivalent, walking the script's member-function table and then
    /// its parents'. Same reasoning as `HasGlobalFunction`: the VM is no kinder
    /// about a missing member function than about a missing global one, and a
    /// third-party script is free to rename `SetFollowerHome` in its next release.
    bool HasMethod(const std::string& scriptName, const std::string& functionName,
                   std::size_t argCount);

    // ── Static (global) calls ─────────────────────────────────────────────
    bool CallGlobalFunction(const std::string& scriptName, const std::string& functionName,
                            const std::vector<PapyrusValue>& args = {});

    /// Async, non-blocking. The callback receives the result on the VM thread.
    bool CallGlobalFunctionInt(const std::string& scriptName, const std::string& functionName,
                               const std::vector<PapyrusValue>& args, IntCallback callback);

    /// Real implementation, not a stub. The source project's version always
    /// returned `nullopt` regardless of what the script produced.
    bool CallGlobalFunctionString(const std::string& scriptName, const std::string& functionName,
                                  const std::vector<PapyrusValue>& args, StringCallback callback);

    bool CallGlobalFunctionBool(const std::string& scriptName, const std::string& functionName,
                                const std::vector<PapyrusValue>& args, BoolCallback callback);

    bool CallGlobalFunctionForm(const std::string& scriptName, const std::string& functionName,
                                const std::vector<PapyrusValue>& args, FormCallback callback);

    bool CallGlobalFunctionStringArray(const std::string& scriptName,
                                       const std::string& functionName,
                                       const std::vector<PapyrusValue>& args,
                                       StringArrayCallback callback);

    // ── Method calls on a form ────────────────────────────────────────────
    /// `vm->DispatchMethodCall2(handle, className, fnName, args, callback)`.
    ///
    /// This is how the plugin reaches another mod's own logic rather than
    /// reimplementing it - `Actor.SetRelationshipRank`,
    /// `vvvMarkHomeQuest.ForceAlias`, `_JSW_BB_Storage.TrackedActorAdd`.
    bool CallMethod(RE::TESForm* target, const std::string& scriptName,
                    const std::string& functionName, const std::vector<PapyrusValue>& args = {});

    bool CallMethodInt(RE::TESForm* target, const std::string& scriptName,
                       const std::string& functionName, const std::vector<PapyrusValue>& args,
                       IntCallback callback);

    bool CallMethodBool(RE::TESForm* target, const std::string& scriptName,
                        const std::string& functionName, const std::vector<PapyrusValue>& args,
                        BoolCallback callback);

    // ── Method calls on an alias ──────────────────────────────────────────
    /// Uses the raw `GetHandleForObject(VMTypeID, const void*)` overload, because
    /// an alias is not a `TESForm` and the convenient `FormType` overload cannot
    /// express it. Required for `ReferenceAlias.ForceRefTo`, which has no
    /// CommonLib binding at all.
    bool CallAliasMethod(RE::BGSBaseAlias* alias, const std::string& scriptName,
                         const std::string& functionName,
                         const std::vector<PapyrusValue>& args = {});

    // ── ModEvent bridge ───────────────────────────────────────────────────
    /// `ModEvent.Create` / `PushForm` / `PushString` / `PushInt` / `PushFloat` /
    /// `PushBool` / `Send`, driven as static calls.
    ///
    /// `SKSE::ModCallbackEventSource::SendEvent` **cannot** substitute for this: it
    /// emits exactly `(string eventName, string strArg, float numArg, Form sender)`
    /// and so cannot drive a multi-argument event such as Fertility Mode's.
    /// Requires SKSE's `ModEvent` script (from PapyrusUtil / SKSE scripts).
    bool SendModEvent(const std::string& eventName, const std::vector<PapyrusValue>& args);

    /// Pack a value into a Variable. Public because the argument adapter needs it.
    void PackVariable(RE::BSScript::Variable& var, const PapyrusValue& value);

private:
    PapyrusInterface() = default;
    ~PapyrusInterface() = default;
    PapyrusInterface(const PapyrusInterface&) = delete;
    PapyrusInterface& operator=(const PapyrusInterface&) = delete;

    /// "Script::Function/argc" -> callable. Globals and methods live in the same
    /// map under different key shapes. Guarded because categories collect from the
    /// worker as well as the game thread.
    std::unordered_map<std::string, bool> m_functionCache;
    std::mutex m_functionCacheMutex;

    /// Shared body of `HasGlobalFunction` and `HasMethod`.
    ///
    /// `walkParents` is the only difference in behaviour: a global function is
    /// declared on exactly one script, a member function is inherited.
    bool HasFunction(const std::string& scriptName, const std::string& functionName,
                     std::size_t argCount, bool memberFunctions, bool walkParents);

    void PackForm(RE::BSScript::Variable& var, RE::TESForm* form, const char* className);
    RE::BSTSmartPointer<RE::BSScript::Array> CreateActorArray(const std::vector<RE::Actor*>& actors);
    RE::BSTSmartPointer<RE::BSScript::Array> CreateStringArray(
        const std::vector<std::string>& strings);

    /// Resolve a form to a VM handle, creating and binding the script object if
    /// this is the first time the VM has seen it.
    bool ResolveHandle(RE::TESForm* form, const std::string& scriptName,
                       RE::BSScript::Internal::VirtualMachine*& vmOut, RE::VMHandle& handleOut);
};

/// A sequencer of steps, one game-thread task each.
///
/// Multi-call restore procedures - MHIYH's "ForceAlias, then read back the index,
/// then move seven markers" - read linearly here instead of nesting callbacks, and
/// with no blocking wait anywhere. Each step returns true to continue or false to
/// abort the remainder.
class PapyrusStepQueue {
public:
    using Step = std::function<bool()>;

    explicit PapyrusStepQueue(std::string label) : m_label(std::move(label)) {}

    /// Steps run in the order added, one per frame.
    PapyrusStepQueue& Then(std::string name, Step step);

    /// Start running. The queue owns itself until it finishes.
    static void Run(std::shared_ptr<PapyrusStepQueue> queue);

    [[nodiscard]] const std::string& Label() const { return m_label; }

private:
    void RunNext(std::shared_ptr<PapyrusStepQueue> self);

    std::string m_label;
    std::vector<std::pair<std::string, Step>> m_steps;
    size_t m_index = 0;
};

}  // namespace SaveMigration::Papyrus
