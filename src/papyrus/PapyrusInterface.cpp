#include "papyrus/PapyrusInterface.h"

#include <algorithm>
#include <format>
#include <memory>

#include <RE/P/PackUnpack.h>

#include "util/GameThread.h"
#include "util/StringUtil.h"

namespace SaveMigration::Papyrus {

// ═══════════════════════════════════════════════════════════════════════════
// Async captors
// ═══════════════════════════════════════════════════════════════════════════
//
// Each one records the result and hands it to a std::function. None of them
// blocks: `WaitForResult` on the game thread deadlocks the VM it is waiting on,
// because the game thread is what pumps that VM.

namespace {

template <class T, class Extract>
class AsyncCaptor final : public RE::BSScript::IStackCallbackFunctor {
public:
    AsyncCaptor(std::function<void(T)> callback, Extract extract)
        : m_callback(std::move(callback)), m_extract(std::move(extract)) {}

    void operator()(RE::BSScript::Variable a_result) override {
        if (m_callback) {
            m_callback(m_extract(a_result));
        }
    }
    void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

private:
    std::function<void(T)> m_callback;
    Extract m_extract;
};

int32_t ExtractInt(RE::BSScript::Variable& var) { return var.IsInt() ? var.GetSInt() : -1; }

std::string ExtractString(RE::BSScript::Variable& var) {
    if (!var.IsString()) {
        return {};
    }
    const auto sv = var.GetString();
    return std::string(sv.data(), sv.size());
}

bool ExtractBool(RE::BSScript::Variable& var) { return var.IsBool() && var.GetBool(); }

RE::TESForm* ExtractForm(RE::BSScript::Variable& var) {
    if (!var.IsObject()) {
        return nullptr;
    }
    // Unpack rather than resolving the handle by hand: it consults the type table
    // and works for every form type, where a fixed VMTypeID does not.
    return var.Unpack<RE::TESForm*>();
}

std::vector<std::string> ExtractStringArray(RE::BSScript::Variable& var) {
    std::vector<std::string> result;
    if (!var.IsArray()) {
        return result;
    }
    auto array = var.GetArray();
    if (!array) {
        return result;
    }
    result.reserve(array->size());
    for (uint32_t i = 0; i < array->size(); ++i) {
        auto& element = (*array)[i];
        if (element.IsString()) {
            const auto sv = element.GetString();
            result.emplace_back(sv.data(), sv.size());
        }
    }
    return result;
}

/// Adapter that packs a `std::vector<PapyrusValue>` into the VM's argument array.
class DynamicFunctionArguments final : public RE::BSScript::IFunctionArguments {
public:
    DynamicFunctionArguments(PapyrusInterface* iface, std::vector<PapyrusValue> args)
        : m_interface(iface), m_args(std::move(args)) {}

    bool operator()(RE::BSScrapArray<RE::BSScript::Variable>& a_dst) const override {
        a_dst.resize(m_args.size());
        for (size_t i = 0; i < m_args.size(); ++i) {
            m_interface->PackVariable(a_dst[i], m_args[i]);
        }
        return true;
    }

private:
    PapyrusInterface* m_interface;
    std::vector<PapyrusValue> m_args;
};

template <class T, class Extract>
RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> MakeCaptor(std::function<void(T)> callback,
                                                                   Extract extract) {
    return RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>(
        RE::make_smart<AsyncCaptor<T, Extract>>(std::move(callback), std::move(extract)));
}

}  // namespace

PapyrusInterface* PapyrusInterface::GetSingleton() {
    static PapyrusInterface instance;
    return &instance;
}

RE::BSScript::Internal::VirtualMachine* PapyrusInterface::GetVM() {
    return RE::BSScript::Internal::VirtualMachine::GetSingleton();
}

bool PapyrusInterface::HasGlobalFunction(const std::string& scriptName,
                                         const std::string& functionName) {
    // Dispatching a global function a script does not declare is not a graceful
    // "returns false" - it crashed the game from inside the VM (2026-08-07: TNG has
    // no GetActorAddonForm, and the access violation landed in SkyrimVR.exe).
    // Third-party scripts rename and drop functions between versions, so every
    // dispatch is checked against the script's own table first.
    auto* vm = GetVM();
    if (!vm) {
        return false;
    }

    const auto key = std::format("{}::{}", scriptName, functionName);
    {
        const std::lock_guard lock(m_functionCacheMutex);
        if (const auto it = m_functionCache.find(key); it != m_functionCache.end()) {
            return it->second;
        }
    }

    bool present = false;
    RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typeInfo;
    if (vm->GetScriptObjectType(RE::BSFixedString(scriptName.c_str()), typeInfo) && typeInfo) {
        const auto* iter = typeInfo->GetGlobalFuncIter();
        const auto count = typeInfo->GetNumGlobalFuncs();
        for (std::uint32_t i = 0; i < count && !present; ++i) {
            const auto& func = iter[i].func;
            // Papyrus identifiers are case-insensitive.
            if (func && Util::IEquals(func->GetName().c_str(), functionName)) {
                present = true;
            }
        }
    }

    if (!present) {
        spdlog::warn("PapyrusInterface: script '{}' declares no global function '{}' - skipping "
                     "the call rather than dispatching into the VM",
                     scriptName, functionName);
    }

    const std::lock_guard lock(m_functionCacheMutex);
    m_functionCache[key] = present;
    return present;
}

// ═══════════════════════════════════════════════════════════════════════════
// Packing
// ═══════════════════════════════════════════════════════════════════════════

void PapyrusInterface::PackForm(RE::BSScript::Variable& var, RE::TESForm* form,
                                const char* className) {
    if (!form) {
        var.SetNone();
        return;
    }
    auto* vm = GetVM();
    if (!vm) {
        var.SetNone();
        return;
    }
    auto* policy = vm->GetObjectHandlePolicy();
    if (!policy) {
        var.SetNone();
        return;
    }

    const auto handle = policy->GetHandleForObject(form->GetFormType(), form);
    if (handle == policy->EmptyHandle()) {
        spdlog::warn("PapyrusInterface: no handle for form {:08X}", form->GetFormID());
        var.SetNone();
        return;
    }

    RE::BSTSmartPointer<RE::BSScript::Object> object;
    if (!vm->FindBoundObject(handle, className, object) || !object) {
        vm->CreateObject(className, object);
        if (object) {
            vm->BindObject(object, handle, false);
        }
    }
    if (object) {
        var.SetObject(object);
    } else {
        var.SetNone();
    }
}

RE::BSTSmartPointer<RE::BSScript::Array> PapyrusInterface::CreateActorArray(
    const std::vector<RE::Actor*>& actors) {
    auto* vm = GetVM();
    if (!vm) {
        return nullptr;
    }
    std::vector<RE::Actor*> valid;
    valid.reserve(actors.size());
    for (auto* actor : actors) {
        if (actor) {
            valid.push_back(actor);
        }
    }
    if (valid.empty()) {
        return nullptr;
    }

    RE::BSTSmartPointer<RE::BSScript::Array> array;
    if (!vm->CreateArray(RE::BSScript::TypeInfo::RawType::kObjectArray, "Actor",
                         static_cast<uint32_t>(valid.size()), array) ||
        !array) {
        return nullptr;
    }
    for (size_t i = 0; i < valid.size(); ++i) {
        (*array)[i].Pack(valid[i]);
    }
    return array;
}

RE::BSTSmartPointer<RE::BSScript::Array> PapyrusInterface::CreateStringArray(
    const std::vector<std::string>& strings) {
    auto* vm = GetVM();
    if (!vm || strings.empty()) {
        return nullptr;
    }
    RE::BSTSmartPointer<RE::BSScript::Array> array;
    if (!vm->CreateArray(RE::BSScript::TypeInfo::RawType::kStringArray, "",
                         static_cast<uint32_t>(strings.size()), array) ||
        !array) {
        return nullptr;
    }
    for (size_t i = 0; i < strings.size(); ++i) {
        (*array)[i].SetString(strings[i]);
    }
    return array;
}

void PapyrusInterface::PackVariable(RE::BSScript::Variable& var, const PapyrusValue& value) {
    std::visit(
        [this, &var](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                var.SetNone();
            } else if constexpr (std::is_same_v<T, int>) {
                var.SetSInt(arg);
            } else if constexpr (std::is_same_v<T, float>) {
                var.SetFloat(arg);
            } else if constexpr (std::is_same_v<T, bool>) {
                var.SetBool(arg);
            } else if constexpr (std::is_same_v<T, std::string>) {
                var.SetString(arg);
            } else if constexpr (std::is_same_v<T, RE::Actor*>) {
                PackForm(var, arg, "Actor");
            } else if constexpr (std::is_same_v<T, RE::TESObjectREFR*>) {
                PackForm(var, arg, "ObjectReference");
            } else if constexpr (std::is_same_v<T, RE::TESForm*>) {
                // The source project did `arg->As<RE::TESObjectREFR>()` here, which
                // is nullptr for a Faction, Armor, Race, Outfit or Quest - so every
                // such argument was silently packed as None. `Variable::Pack`
                // consults the type table and gets it right for any form type.
                if (arg) {
                    var.Pack(arg);
                } else {
                    var.SetNone();
                }
            } else if constexpr (std::is_same_v<T, std::vector<RE::Actor*>>) {
                if (auto array = CreateActorArray(arg)) {
                    var.SetArray(array);
                } else {
                    var.SetNone();
                }
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                if (auto array = CreateStringArray(arg)) {
                    var.SetArray(array);
                } else {
                    var.SetNone();
                }
            }
        },
        value);
}

// ═══════════════════════════════════════════════════════════════════════════
// Static calls
// ═══════════════════════════════════════════════════════════════════════════

bool PapyrusInterface::CallGlobalFunction(const std::string& scriptName,
                                         const std::string& functionName,
                                         const std::vector<PapyrusValue>& args) {
    auto* vm = GetVM();
    if (!vm) {
        spdlog::error("PapyrusInterface: no VM for {}::{}", scriptName, functionName);
        return false;
    }
    if (!HasGlobalFunction(scriptName, functionName)) {
        return false;
    }
    DynamicFunctionArguments funcArgs(this, args);
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback;
    const bool ok = vm->DispatchStaticCall(scriptName, functionName, &funcArgs, noCallback);
    spdlog::debug("PapyrusInterface: {}::{}({} args) -> {}", scriptName, functionName, args.size(),
                  ok);
    return ok;
}

bool PapyrusInterface::CallGlobalFunctionInt(const std::string& scriptName,
                                            const std::string& functionName,
                                            const std::vector<PapyrusValue>& args,
                                            IntCallback callback) {
    auto* vm = GetVM();
    if (!vm) {
        return false;
    }
    if (!HasGlobalFunction(scriptName, functionName)) {
        return false;
    }
    DynamicFunctionArguments funcArgs(this, args);
    auto captor = MakeCaptor<int32_t>(std::move(callback), ExtractInt);
    return vm->DispatchStaticCall(scriptName, functionName, &funcArgs, captor);
}

bool PapyrusInterface::CallGlobalFunctionString(const std::string& scriptName,
                                               const std::string& functionName,
                                               const std::vector<PapyrusValue>& args,
                                               StringCallback callback) {
    auto* vm = GetVM();
    if (!vm) {
        return false;
    }
    if (!HasGlobalFunction(scriptName, functionName)) {
        return false;
    }
    DynamicFunctionArguments funcArgs(this, args);
    auto captor = MakeCaptor<const std::string&>(
        [cb = std::move(callback)](const std::string& value) {
            if (cb) {
                cb(value);
            }
        },
        ExtractString);
    return vm->DispatchStaticCall(scriptName, functionName, &funcArgs, captor);
}

bool PapyrusInterface::CallGlobalFunctionBool(const std::string& scriptName,
                                             const std::string& functionName,
                                             const std::vector<PapyrusValue>& args,
                                             BoolCallback callback) {
    auto* vm = GetVM();
    if (!vm) {
        return false;
    }
    if (!HasGlobalFunction(scriptName, functionName)) {
        return false;
    }
    DynamicFunctionArguments funcArgs(this, args);
    auto captor = MakeCaptor<bool>(std::move(callback), ExtractBool);
    return vm->DispatchStaticCall(scriptName, functionName, &funcArgs, captor);
}

bool PapyrusInterface::CallGlobalFunctionForm(const std::string& scriptName,
                                             const std::string& functionName,
                                             const std::vector<PapyrusValue>& args,
                                             FormCallback callback) {
    auto* vm = GetVM();
    if (!vm) {
        return false;
    }
    if (!HasGlobalFunction(scriptName, functionName)) {
        return false;
    }
    DynamicFunctionArguments funcArgs(this, args);
    auto captor = MakeCaptor<RE::TESForm*>(std::move(callback), ExtractForm);
    return vm->DispatchStaticCall(scriptName, functionName, &funcArgs, captor);
}

bool PapyrusInterface::CallGlobalFunctionStringArray(const std::string& scriptName,
                                                    const std::string& functionName,
                                                    const std::vector<PapyrusValue>& args,
                                                    StringArrayCallback callback) {
    auto* vm = GetVM();
    if (!vm) {
        return false;
    }
    if (!HasGlobalFunction(scriptName, functionName)) {
        return false;
    }
    DynamicFunctionArguments funcArgs(this, args);
    auto captor = MakeCaptor<const std::vector<std::string>&>(
        [cb = std::move(callback)](const std::vector<std::string>& value) {
            if (cb) {
                cb(value);
            }
        },
        ExtractStringArray);
    return vm->DispatchStaticCall(scriptName, functionName, &funcArgs, captor);
}

// ═══════════════════════════════════════════════════════════════════════════
// Method calls
// ═══════════════════════════════════════════════════════════════════════════

bool PapyrusInterface::ResolveHandle(RE::TESForm* form, const std::string& scriptName,
                                    RE::BSScript::Internal::VirtualMachine*& vmOut,
                                    RE::VMHandle& handleOut) {
    vmOut = GetVM();
    if (!vmOut || !form) {
        return false;
    }
    auto* policy = vmOut->GetObjectHandlePolicy();
    if (!policy) {
        return false;
    }
    handleOut = policy->GetHandleForObject(form->GetFormType(), form);
    if (handleOut == policy->EmptyHandle()) {
        spdlog::warn("PapyrusInterface: no handle for {:08X} calling {}", form->GetFormID(),
                     scriptName);
        return false;
    }
    return true;
}

bool PapyrusInterface::CallMethod(RE::TESForm* target, const std::string& scriptName,
                                 const std::string& functionName,
                                 const std::vector<PapyrusValue>& args) {
    RE::BSScript::Internal::VirtualMachine* vm = nullptr;
    RE::VMHandle handle{};
    if (!ResolveHandle(target, scriptName, vm, handle)) {
        return false;
    }
    DynamicFunctionArguments funcArgs(this, args);
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback;
    const bool ok =
        vm->DispatchMethodCall2(handle, scriptName, functionName, &funcArgs, noCallback);
    spdlog::debug("PapyrusInterface: {:08X}.{}::{}({} args) -> {}", target->GetFormID(), scriptName,
                  functionName, args.size(), ok);
    return ok;
}

bool PapyrusInterface::CallMethodInt(RE::TESForm* target, const std::string& scriptName,
                                    const std::string& functionName,
                                    const std::vector<PapyrusValue>& args, IntCallback callback) {
    RE::BSScript::Internal::VirtualMachine* vm = nullptr;
    RE::VMHandle handle{};
    if (!ResolveHandle(target, scriptName, vm, handle)) {
        return false;
    }
    DynamicFunctionArguments funcArgs(this, args);
    auto captor = MakeCaptor<int32_t>(std::move(callback), ExtractInt);
    return vm->DispatchMethodCall2(handle, scriptName, functionName, &funcArgs, captor);
}

bool PapyrusInterface::CallMethodBool(RE::TESForm* target, const std::string& scriptName,
                                     const std::string& functionName,
                                     const std::vector<PapyrusValue>& args, BoolCallback callback) {
    RE::BSScript::Internal::VirtualMachine* vm = nullptr;
    RE::VMHandle handle{};
    if (!ResolveHandle(target, scriptName, vm, handle)) {
        return false;
    }
    DynamicFunctionArguments funcArgs(this, args);
    auto captor = MakeCaptor<bool>(std::move(callback), ExtractBool);
    return vm->DispatchMethodCall2(handle, scriptName, functionName, &funcArgs, captor);
}

bool PapyrusInterface::CallAliasMethod(RE::BGSBaseAlias* alias, const std::string& scriptName,
                                      const std::string& functionName,
                                      const std::vector<PapyrusValue>& args) {
    auto* vm = GetVM();
    if (!vm || !alias) {
        return false;
    }
    auto* policy = vm->GetObjectHandlePolicy();
    if (!policy) {
        return false;
    }

    // An alias is not a TESForm, so the convenient FormType overload cannot
    // express it. The raw (VMTypeID, const void*) overload can, and the alias
    // carries its own VMTypeID.
    const auto handle = policy->GetHandleForObject(alias->GetVMTypeID(), alias);
    if (handle == policy->EmptyHandle()) {
        spdlog::warn("PapyrusInterface: no handle for alias '{}' calling {}::{}",
                     alias->aliasName.c_str(), scriptName, functionName);
        return false;
    }

    DynamicFunctionArguments funcArgs(this, args);
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback;
    const bool ok =
        vm->DispatchMethodCall2(handle, scriptName, functionName, &funcArgs, noCallback);
    spdlog::debug("PapyrusInterface: alias '{}'.{}::{} -> {}", alias->aliasName.c_str(), scriptName,
                  functionName, ok);
    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════
// ModEvent bridge
// ═══════════════════════════════════════════════════════════════════════════

bool PapyrusInterface::SendModEvent(const std::string& eventName,
                                   const std::vector<PapyrusValue>& args) {
    auto* vm = GetVM();
    if (!vm) {
        return false;
    }

    // ModEvent.Create returns an int handle. Everything after it has to happen
    // *after* that result arrives, which is why the pushes are chained inside the
    // captor rather than issued straight away.
    const bool dispatched = CallGlobalFunctionInt(
        "ModEvent", "Create", {eventName}, [this, eventName, args](int32_t handle) {
            if (handle == 0 || handle == -1) {
                spdlog::warn("PapyrusInterface: ModEvent.Create('{}') returned no handle - is the "
                             "SKSE ModEvent script installed?",
                             eventName);
                return;
            }
            // Pushes and Send must run on the game thread: they are VM calls, and
            // we are currently on the VM's callback thread.
            Util::OnGameThread([this, handle, eventName, args]() {
                for (const auto& arg : args) {
                    std::visit(
                        [this, handle](auto&& value) {
                            using T = std::decay_t<decltype(value)>;
                            if constexpr (std::is_same_v<T, int>) {
                                CallGlobalFunction("ModEvent", "PushInt", {handle, value});
                            } else if constexpr (std::is_same_v<T, float>) {
                                CallGlobalFunction("ModEvent", "PushFloat", {handle, value});
                            } else if constexpr (std::is_same_v<T, bool>) {
                                CallGlobalFunction("ModEvent", "PushBool", {handle, value});
                            } else if constexpr (std::is_same_v<T, std::string>) {
                                CallGlobalFunction("ModEvent", "PushString", {handle, value});
                            } else if constexpr (std::is_same_v<T, RE::TESForm*> ||
                                                 std::is_same_v<T, RE::Actor*> ||
                                                 std::is_same_v<T, RE::TESObjectREFR*>) {
                                CallGlobalFunction("ModEvent", "PushForm",
                                                   {handle, static_cast<RE::TESForm*>(value)});
                            }
                        },
                        arg);
                }
                CallGlobalFunction("ModEvent", "Send", {handle});
                spdlog::debug("PapyrusInterface: sent ModEvent '{}' with {} arg(s)", eventName,
                              args.size());
            });
        });

    if (!dispatched) {
        spdlog::warn("PapyrusInterface: could not dispatch ModEvent.Create for '{}'", eventName);
    }
    return dispatched;
}

// ═══════════════════════════════════════════════════════════════════════════
// PapyrusStepQueue
// ═══════════════════════════════════════════════════════════════════════════

PapyrusStepQueue& PapyrusStepQueue::Then(std::string name, Step step) {
    m_steps.emplace_back(std::move(name), std::move(step));
    return *this;
}

void PapyrusStepQueue::Run(std::shared_ptr<PapyrusStepQueue> queue) {
    if (!queue || queue->m_steps.empty()) {
        return;
    }
    queue->RunNext(queue);
}

void PapyrusStepQueue::RunNext(std::shared_ptr<PapyrusStepQueue> self) {
    if (m_index >= m_steps.size()) {
        spdlog::debug("PapyrusStepQueue['{}']: finished", m_label);
        return;
    }

    const auto index = m_index++;
    const auto& [name, step] = m_steps[index];

    // One game-thread task per step. Anything the step dispatches to the VM
    // completes before the next frame, which is what makes a read-back in the
    // following step see the result.
    Util::OnGameThread([this, self, index, name = name, step = step]() {
        bool keepGoing = true;
        try {
            keepGoing = step();
        } catch (const std::exception& e) {
            spdlog::error("PapyrusStepQueue['{}']: step '{}' threw: {}", m_label, name, e.what());
            keepGoing = false;
        }
        if (!keepGoing) {
            spdlog::warn("PapyrusStepQueue['{}']: aborted at step {} ('{}')", m_label, index, name);
            return;
        }
        RunNext(self);
    });
}

}  // namespace SaveMigration::Papyrus
