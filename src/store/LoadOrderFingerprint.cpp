#include "store/LoadOrderFingerprint.h"

#include <algorithm>
#include <format>

#include "model/FormKeyUtil.h"
#include "util/StringUtil.h"

namespace SaveMigration::Store {

nlohmann::json PluginRecord::ToJson() const {
    return nlohmann::json{
        {"filename", filename},
        {"isLight", isLight},
        {"compileIndex", compileIndex},
        {"smallFileCompileIndex", smallFileCompileIndex},
        {"fileSize", fileSize},
    };
}

PluginRecord PluginRecord::FromJson(const nlohmann::json& json) {
    PluginRecord record;
    record.filename = json.value("filename", "");
    record.isLight = json.value("isLight", false);
    record.compileIndex = json.value("compileIndex", 0u);
    record.smallFileCompileIndex = json.value("smallFileCompileIndex", 0u);
    record.fileSize = json.value("fileSize", 0ull);
    return record;
}

LoadOrderFingerprint& LoadOrderFingerprint::Get() {
    static LoadOrderFingerprint instance;
    return instance;
}

void LoadOrderFingerprint::CaptureCurrent() {
    m_current.clear();

    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        spdlog::error("LoadOrderFingerprint: TESDataHandler unavailable");
        return;
    }

    for (const auto* file : handler->files) {
        if (!file) {
            continue;
        }
        PluginRecord record;
        record.filename = file->GetFilename();
        record.isLight = file->IsLight();
        record.compileIndex = file->GetCompileIndex();
        record.smallFileCompileIndex = file->GetSmallFileCompileIndex();
        record.fileSize = file->filesize;
        m_current.push_back(std::move(record));
    }

    m_captured = true;
    spdlog::info("LoadOrderFingerprint: captured {} plugins", m_current.size());
}

nlohmann::json LoadOrderFingerprint::ToJson() const {
    auto plugins = nlohmann::json::array();
    for (const auto& record : m_current) {
        plugins.push_back(record.ToJson());
    }
    return nlohmann::json{
        {"runtimeIsVR", REL::Module::IsVR()},
        {"count", m_current.size()},
        {"plugins", std::move(plugins)},
    };
}

std::vector<PluginRecord> LoadOrderFingerprint::FromJson(const nlohmann::json& json) {
    std::vector<PluginRecord> records;
    if (!json.is_object()) {
        return records;
    }
    const auto plugins = json.find("plugins");
    if (plugins == json.end() || !plugins->is_array()) {
        return records;
    }
    for (const auto& entry : *plugins) {
        records.push_back(PluginRecord::FromJson(entry));
    }
    return records;
}

LoadOrderFingerprint::Diff LoadOrderFingerprint::DiffAgainst(
    const std::vector<PluginRecord>& snapshot) const {
    Diff diff;

    const auto findLive = [this](std::string_view name) -> const PluginRecord* {
        for (const auto& record : m_current) {
            if (Util::IEquals(record.filename, name)) {
                return &record;
            }
        }
        return nullptr;
    };
    const auto inSnapshot = [&snapshot](std::string_view name) {
        return std::any_of(snapshot.begin(), snapshot.end(), [&](const PluginRecord& record) {
            return Util::IEquals(record.filename, name);
        });
    };

    for (const auto& record : snapshot) {
        const auto* live = findLive(record.filename);
        if (!live) {
            diff.missing.push_back(record.filename);
        }
        // File size is deliberately not compared. It changes whenever a mod is
        // updated, which tells us nothing about whether a record this snapshot
        // names has moved - and a form that has moved fails its own lookup, in
        // front of the item the player was expecting.
    }
    for (const auto& record : m_current) {
        if (!inSnapshot(record.filename)) {
            diff.added.push_back(record.filename);
        }
    }

    spdlog::info("LoadOrderFingerprint: diff - {} missing, {} added", diff.missing.size(),
                 diff.added.size());
    return diff;
}

std::string LoadOrderFingerprint::OldRuntimeIdToFormKey(
    uint32_t oldRuntimeFormId, const std::vector<PluginRecord>& snapshotOrder) {
    // Dynamic forms carry no plugin identity at all.
    if ((oldRuntimeFormId & 0xFF000000u) == 0xFF000000u) {
        return "";
    }

    // An 0xFE-prefixed ID can only be an ESL, and only on a runtime that has
    // ESL space. When the snapshot came from VR there is no ESL space, so an
    // 0xFE top byte there is a plain compile index of 0xFE.
    const bool eslShaped = (oldRuntimeFormId & 0xFF000000u) == 0xFE000000u;
    if (eslShaped) {
        const uint32_t smallIndex = (oldRuntimeFormId & 0x00FFF000u) >> 12;
        const uint32_t localId = oldRuntimeFormId & 0x00000FFFu;
        for (const auto& record : snapshotOrder) {
            if (record.isLight && record.smallFileCompileIndex == smallIndex) {
                return Model::FormKeyUtil::BuildFormKey(localId, record.filename);
            }
        }
    }

    const uint32_t compileIndex = (oldRuntimeFormId & 0xFF000000u) >> 24;
    const uint32_t localId = oldRuntimeFormId & 0x00FFFFFFu;
    for (const auto& record : snapshotOrder) {
        if (!record.isLight && record.compileIndex == compileIndex) {
            return Model::FormKeyUtil::BuildFormKey(localId, record.filename);
        }
    }

    // A light plugin loaded on VR occupies a normal compile index slot, so the
    // non-light filter above can miss it. Retry without the filter.
    for (const auto& record : snapshotOrder) {
        if (record.compileIndex == compileIndex) {
            return Model::FormKeyUtil::BuildFormKey(localId, record.filename);
        }
    }

    return "";
}

}  // namespace SaveMigration::Store
