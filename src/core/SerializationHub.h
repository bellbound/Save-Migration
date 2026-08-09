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

    /// True when SKSE's co-save *load* callback has run since the last revert.
    ///
    /// SKSE issues a revert for both a savegame load and a new game, but the
    /// load callback only for a savegame load. So a false here means this
    /// session's game was started fresh rather than loaded — which is the one
    /// question `kNewGame` was being asked to answer, and answers it for the
    /// ways of starting a game that send no `kNewGame` (`coc` from the main
    /// menu being the one that matters).
    [[nodiscard]] bool CoSaveLoadRan() const { return m_coSaveLoadRan; }

    /// Bumped on every revert — that is, on every savegame load and every new
    /// game, and therefore every time the engine tears the world down and frees
    /// every non-persistent reference in it.
    ///
    /// Anything that holds a raw `RE::TESForm*` across more than one frame must
    /// compare this before dereferencing. The restore's apply pass is the case
    /// that matters: it resolves the whole roster once, then walks it one phase
    /// per frame over what can be several seconds of real time, and the player is
    /// in control for all of it. A quickload in the middle of that leaves a
    /// `shared_ptr<RunState>` full of pointers to actors the engine has already
    /// destroyed, and the next queued phase reads them.
    ///
    /// The deferred queue does not need this: it stores FormKeys and resolves
    /// them afresh on every drain, which is why it is safe across sessions.
    [[nodiscard]] static uint64_t SessionEpoch();

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
    bool m_coSaveLoadRan = false;
};

}  // namespace SaveMigration::Core
