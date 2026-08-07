#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace SaveMigration::Store {

/// One plugin, as recorded in `loadorder.json`.
struct PluginRecord {
    std::string filename;
    bool isLight = false;
    uint32_t compileIndex = 0;
    uint32_t smallFileCompileIndex = 0;
    uint64_t fileSize = 0;

    [[nodiscard]] nlohmann::json ToJson() const;
    static PluginRecord FromJson(const nlohmann::json& json);
};

/// The load order at snapshot time, and the diff against the load order at
/// restore time.
///
/// This is not decoration. The SkyrimNet `form_id` repair needs the *old* plugin
/// index table to translate a stored runtime FormID back into a plugin plus
/// local ID - that translation is impossible without it. The missing/added diff
/// then drives pre-failing every key from a plugin that is no longer here.
class LoadOrderFingerprint {
public:
    static LoadOrderFingerprint& Get();

    /// Walk `TESDataHandler::files`. Game thread; call at kDataLoaded.
    void CaptureCurrent();

    [[nodiscard]] const std::vector<PluginRecord>& Current() const { return m_current; }
    [[nodiscard]] bool IsCaptured() const { return m_captured; }

    [[nodiscard]] nlohmann::json ToJson() const;
    static std::vector<PluginRecord> FromJson(const nlohmann::json& json);

    struct Diff {
        /// In the snapshot, absent now. Keys from these are pre-failed.
        std::vector<std::string> missing;
        /// Here now, absent from the snapshot. Harmless, but worth reporting -
        /// a newly added mod may fight a restore.
        std::vector<std::string> added;
        /// Present in both but with a different size, i.e. a different version.
        /// Local form IDs may have shifted inside it.
        std::vector<std::string> changed;
    };

    /// Compare a snapshot's recorded order against the live one.
    [[nodiscard]] Diff DiffAgainst(const std::vector<PluginRecord>& snapshot) const;

    /// Resolve an *old-session* runtime FormID to a FormKey, using the recorded
    /// order. This is the SkyrimNet repair's core operation and is only
    /// meaningful with the snapshot's own table.
    ///
    /// The VR/SE ESL split is handled explicitly here rather than deferred to
    /// CommonLib, because we are decoding an ID minted by a *past* session whose
    /// runtime we know from the record, not the current one.
    [[nodiscard]] static std::string OldRuntimeIdToFormKey(
        uint32_t oldRuntimeFormId, const std::vector<PluginRecord>& snapshotOrder);

private:
    LoadOrderFingerprint() = default;

    std::vector<PluginRecord> m_current;
    bool m_captured = false;
};

}  // namespace SaveMigration::Store
