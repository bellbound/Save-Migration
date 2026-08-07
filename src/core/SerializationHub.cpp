#include "core/SerializationHub.h"

#include <algorithm>

namespace SaveMigration::Core {

SerializationHub& SerializationHub::Get() {
    static SerializationHub instance;
    return instance;
}

void SerializationHub::RegisterHandler(IRecordHandler* handler) {
    if (!handler) {
        return;
    }
    if (FindHandler(handler->Signature())) {
        spdlog::error("SerializationHub: duplicate signature for handler '{}' - ignoring",
                      handler->Name());
        return;
    }
    m_handlers.push_back(handler);
    spdlog::debug("SerializationHub: registered record '{}' (v{})", handler->Name(),
                  handler->Version());
}

IRecordHandler* SerializationHub::FindHandler(uint32_t signature) {
    for (auto* handler : m_handlers) {
        if (handler->Signature() == signature) {
            return handler;
        }
    }
    return nullptr;
}

void SerializationHub::Initialize(const SKSE::SerializationInterface* serialization) {
    if (!serialization) {
        spdlog::error("SerializationHub: no serialization interface - co-save state disabled");
        return;
    }
    if (m_initialized) {
        return;
    }

    // The SKSE interface is handed to plugins as const but the registration
    // methods are non-const. Every implementation in this workspace casts it
    // away; doing the same keeps us on the well-travelled path.
    auto* intfc = const_cast<SKSE::SerializationInterface*>(serialization);
    intfc->SetUniqueID(kPluginId);
    intfc->SetSaveCallback(OnSave);
    intfc->SetLoadCallback(OnLoad);
    intfc->SetRevertCallback(OnRevert);

    m_initialized = true;
    spdlog::info("SerializationHub: initialized with {} record(s)", m_handlers.size());
}

void SerializationHub::OnSave(SKSE::SerializationInterface* intfc) {
    auto& self = Get();
    for (auto* handler : self.m_handlers) {
        if (!intfc->OpenRecord(handler->Signature(), handler->Version())) {
            spdlog::error("SerializationHub: OpenRecord failed for '{}'", handler->Name());
            continue;
        }
        handler->Save(intfc);
    }
}

void SerializationHub::OnLoad(SKSE::SerializationInterface* intfc) {
    auto& self = Get();
    std::unordered_set<uint32_t> seen;

    uint32_t type = 0;
    uint32_t version = 0;
    uint32_t length = 0;
    while (intfc->GetNextRecordInfo(type, version, length)) {
        auto* handler = self.FindHandler(type);
        if (!handler) {
            // Records from an older build of this plugin that no longer exist.
            // Skipping is correct; SKSE advances past the payload for us.
            spdlog::debug("SerializationHub: skipping unknown record {:08X} (len {})", type, length);
            continue;
        }
        if (version > handler->Version()) {
            spdlog::warn("SerializationHub: record '{}' is v{} but this build reads at most v{} - skipping",
                         handler->Name(), version, handler->Version());
            continue;
        }
        if (!handler->Load(intfc, version, length)) {
            spdlog::warn("SerializationHub: record '{}' failed to load; leaving it empty",
                         handler->Name());
            handler->Revert();
            continue;
        }
        seen.insert(type);
    }

    // PostLoad runs for *every* handler, present or not: absence is itself
    // information. A missing SMID means this save predates the plugin, which is
    // one half of the "new playthrough" detector.
    for (auto* handler : self.m_handlers) {
        handler->PostLoad(seen.contains(handler->Signature()));
    }
}

void SerializationHub::OnRevert(SKSE::SerializationInterface*) {
    auto& self = Get();
    for (auto* handler : self.m_handlers) {
        handler->Revert();
    }
}

bool SerializationHub::WriteString(SKSE::SerializationInterface* intfc, const std::string& str) {
    const auto length = static_cast<uint32_t>(std::min<size_t>(str.size(), kMaxStringLength));
    if (!intfc->WriteRecordData(&length, sizeof(length))) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    return intfc->WriteRecordData(str.data(), length);
}

bool SerializationHub::ReadString(SKSE::SerializationInterface* intfc, std::string& out,
                                  uint32_t maxLength) {
    out.clear();

    uint32_t length = 0;
    if (!intfc->ReadRecordData(&length, sizeof(length))) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (length > maxLength) {
        // Do not trust a length from a corrupt co-save. Bail rather than
        // allocate; the caller treats a false return as "clear and log".
        spdlog::error("SerializationHub: string length {} exceeds bound {} - refusing", length,
                      maxLength);
        return false;
    }

    out.resize(length);
    if (!intfc->ReadRecordData(out.data(), length)) {
        out.clear();
        return false;
    }
    return true;
}

}  // namespace SaveMigration::Core
