#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace SaveMigration::Core {

/// Build an SKSE record signature from four characters.
/// The value is written to the co-save as a little-endian uint32, so the bytes
/// appear reversed in a hex dump. That is cosmetic; only uniqueness and
/// consistency matter.
constexpr uint32_t MakeSig(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
}

/// One co-save record. Implementations are singletons owned by their module;
/// the hub only borrows them.
class IRecordHandler {
public:
    virtual ~IRecordHandler() = default;

    [[nodiscard]] virtual uint32_t Signature() const = 0;
    [[nodiscard]] virtual uint32_t Version() const = 0;
    [[nodiscard]] virtual const char* Name() const = 0;

    /// Append this record. The hub has already opened it.
    virtual void Save(SKSE::SerializationInterface* intfc) = 0;

    /// Read this record. `version` is what was written; a handler that cannot
    /// read an old version should return false and leave itself empty rather
    /// than half-populated.
    virtual bool Load(SKSE::SerializationInterface* intfc, uint32_t version, uint32_t length) = 0;

    /// Wipe in-memory state. Called on every game load and on new game.
    virtual void Revert() = 0;

    /// Called after the whole co-save has been walked. `wasPresent` says
    /// whether this handler's record actually existed in the file — which is
    /// how "is this a save that predates the plugin?" is answered.
    virtual void PostLoad(bool /*wasPresent*/) {}
};

/// The single owner of this plugin's SKSE serialization callbacks.
///
/// SKSE allows one set of callbacks per plugin, so having several managers each
/// try to register is a silent overwrite. Everything funnels through here.
class SerializationHub {
public:
    /// 'SMPS' — Save Migration Plugin Serialization.
    static constexpr uint32_t kPluginId = MakeSig('S', 'M', 'P', 'S');

    static SerializationHub& Get();

    /// Register handlers first, then call this once from SKSEPluginLoad.
    void Initialize(const SKSE::SerializationInterface* serialization);

    /// Order matters only for readability; records are matched by signature.
    void RegisterHandler(IRecordHandler* handler);

    [[nodiscard]] bool IsInitialized() const { return m_initialized; }

    // ── Length-prefixed string helpers ────────────────────────────────────
    // Kept from VR-Sex-Menu's SaveGameDataManager, including the bound: a
    // corrupt co-save must not be able to make us allocate an arbitrary amount
    // or read past the record.
    static constexpr uint32_t kMaxStringLength = 4096;

    static bool WriteString(SKSE::SerializationInterface* intfc, const std::string& str);
    static bool ReadString(SKSE::SerializationInterface* intfc, std::string& out,
                           uint32_t maxLength = kMaxStringLength);

private:
    SerializationHub() = default;

    static void OnSave(SKSE::SerializationInterface* intfc);
    static void OnLoad(SKSE::SerializationInterface* intfc);
    static void OnRevert(SKSE::SerializationInterface* intfc);

    IRecordHandler* FindHandler(uint32_t signature);

    std::vector<IRecordHandler*> m_handlers;
    bool m_initialized = false;
};

}  // namespace SaveMigration::Core
