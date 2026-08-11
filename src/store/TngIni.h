#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace SaveMigration::Store {

/// Re-expresses The New Gentleman's settings file as JSON, and merges a
/// recorded one back.
///
/// **Why this exists at all.** TNG splits its state two ways. Verified against
/// `TheNewGentleman.dll` and a live `TheNewGentleman5.ini` on 2026-08-10:
///
///   - NPC addon and size choices live in flat sections keyed by the actor's base
///     record. They are save-agnostic, so they already apply to every save on the
///     install - but only on *that* install. The file is per modlist, not per
///     machine and not per save, so a different modlist has entirely different
///     ones and this half does need migrating.
///   - The player's choice is scoped per save line, in a section whose name ends
///     with the eight hex digits Skyrim puts in the save file name.
///
/// Recording the values as data - rather than copying the file - is what makes an
/// import possible at all. Every id in it names a record in the *exporting*
/// modlist, so it has to be re-validated against the importing one before
/// anything is written, and that can only be done entry by entry. Copying the
/// file would also clobber settings that belong to the install rather than the
/// character: valid skeletons, hotkeys, the revealing-mod list, log level.
///
/// **Everything here degrades rather than fails.** This reads another mod's
/// private file format, which is free to change under us and has already changed
/// once - the DLL still carries a `TransferOldIni` routine for an older name. So:
/// the file is *found* by pattern rather than by exact name; sections are matched
/// by prefix, case-insensitively; every entry keeps its raw key and value
/// verbatim alongside any fields we managed to parse out of it; a line that does
/// not fit the expected shape is recorded with `parsed: false` rather than
/// dropped; and section names we do not recognise are listed so that a format
/// change shows up in the export instead of being silently absorbed.
///
/// Files and text only. Nothing here touches game state, which is what keeps the
/// validation - the part that needs `TESDataHandler` and therefore the game
/// thread - in `TngIniCategory` where it belongs.
class TngIni {
public:
    /// Bounds. A corrupt or hostile file must not be able to make us allocate
    /// without limit, on the same principle as the co-save reader's bounds.
    static constexpr uint64_t kMaxIniBytes = 8ull * 1024 * 1024;
    static constexpr size_t kMaxEntriesPerSection = 4096;

    /// Section names, matched case-insensitively and by prefix.
    ///
    /// TNG builds the player section as `"PlayerInfo" + <save token>`, so that one
    /// is necessarily a prefix match. The other two are exact names today and are
    /// still matched the same way, so a suffixed variant is recognised rather than
    /// silently landing in `sectionsNotCaptured`.
    static constexpr std::string_view kPlayerSectionPrefix = "PlayerInfo";
    static constexpr std::string_view kNpcAddonSection = "NPCGenitalAddon";
    static constexpr std::string_view kNpcSizeSection = "NPCGenitalSize";

    /// How many size categories TNG has, and therefore the exclusive upper bound
    /// on a size we are willing to write into its file.
    ///
    /// Mirrors `kSizeCategoryCount` in `NpcTng.cpp`, and both come from the same
    /// fact: TNG defines five size keywords, `0xFE1`..`0xFE5` in
    /// `TheNewGentleman.esp`. Bounded on the way in because the value came from
    /// another modlist's file and TNG indexes its own array with it - writing a 9
    /// would hand TNG an out-of-range index, which is its crash rather than ours
    /// but ours to have caused.
    static constexpr int32_t kSizeCategoryCount = 5;

    struct Result {
        /// False only for a genuine error - unreadable or implausible file. A
        /// missing INI is a successful capture of nothing.
        bool success = false;
        bool found = false;
        std::string error;

        /// Which file was used, relative to `Data`, for the report.
        std::string usedFile;
        /// Whether a section matching this save line was identified.
        bool matchedPlayerSection = false;
        /// How it was identified — `"save id"`, `"character name"`, or empty.
        std::string playerMatchedBy;

        uint32_t playerEntries = 0;
        uint32_t npcEntries = 0;
        /// Entries kept verbatim because they did not fit the expected shape.
        uint32_t unparsedEntries = 0;

        nlohmann::json document;
    };

    /// `saveToken` is the eight hex digits from the save file name, used to pick
    /// the player's section. `characterName` is the fallback when there is no
    /// token to match: a hand-renamed save has no id in its name at all, and that
    /// is not a rare case - it is what a test save looks like.
    ///
    /// Neither being available is still not an error. Every player section is then
    /// captured and the document says plainly that the match is unknown, which is
    /// recoverable by hand; guessing would not be.
    [[nodiscard]] static Result Capture(std::string_view saveToken,
                                        std::string_view characterName);

    /// The eight-hex-digit token out of a save path or file name, e.g.
    /// `Save8_E223B073_0_...ess` -> `E223B073`. Empty when no such token is
    /// present, which is the honest answer for a name we do not recognise.
    [[nodiscard]] static std::string SaveTokenFromPath(std::string_view savePath);

    /// The INI this run would read, relative to `Data`, or empty. Exposed so the
    /// report can name it without re-parsing.
    [[nodiscard]] static std::filesystem::path Locate();

    // ── The import direction ──────────────────────────────────────────────
    //
    // Split in two on purpose. `ReadLive` and `Merge` are text and files;
    // deciding *what* to merge means resolving form keys against the load order,
    // which needs the game thread, so it lives in `TngIniCategory`. Neither half
    // can be tested by reading the other.

    struct Entry {
        std::string key;
        std::string value;
    };

    struct Section {
        std::string name;
        std::vector<Entry> entries;
    };

    /// The settings file as it stands on *this* modlist.
    struct LiveFile {
        /// False for a genuine read failure. A missing file is a successful read
        /// of nothing, because TNG writes the file lazily and its absence only
        /// means it has not had reason to yet.
        bool success = false;
        bool found = false;
        std::string error;
        /// Relative to `Data`, for the report.
        std::string usedFile;
        std::vector<Section> sections;

        /// The entries of `section`, or null. Case-insensitive, exact name.
        [[nodiscard]] const Section* Find(std::string_view section) const;
    };

    [[nodiscard]] static LiveFile ReadLive();

    /// One line to put in the file. `section` is an exact name, not a prefix -
    /// by this point the caller has decided which player section is the one.
    struct Upsert {
        std::string section;
        std::string key;
        std::string value;
    };

    struct MergeResult {
        bool success = false;
        std::string error;
        std::string targetFile;
        /// Where the previous contents were copied first, or empty when there
        /// were none. Inside the snapshot, deliberately - see `Merge`.
        std::string backupFile;

        uint32_t added = 0;
        /// The target had the key with a different value. TNG's own answer loses,
        /// which is the same rule every other category follows: an import writes
        /// the snapshot's value over the target's.
        uint32_t changed = 0;
        /// The target already agreed, so nothing was written for it.
        uint32_t identical = 0;
        uint32_t sectionsCreated = 0;
    };

    /// Apply `upserts` to TNG's settings file and write it back.
    ///
    /// **Everything the caller did not name is preserved byte for byte**, down to
    /// comments, blank lines, the UTF-8 BOM and the CRLF endings TNG writes. This
    /// is another mod's private file and its own parser is the only one whose
    /// opinion counts, so the edit is done on the raw text rather than by
    /// round-tripping through a parsed model that would normalise all of that away.
    ///
    /// **Fails rather than creating the file.** `Locate` takes the newest
    /// `TheNewGentleman*.ini`, and the digit in that name is a format generation
    /// TNG has already bumped once, so inventing a name means guessing the
    /// generation: guess low and TNG never reads what we wrote, guess high and we
    /// shadow the file it does read. TNG writes this file itself, so its absence is
    /// a fact for the report rather than a gap to fill.
    ///
    /// A merge that would change nothing writes nothing - rewriting a file to
    /// produce identical bytes still moves its timestamp, and `Locate` picks by
    /// timestamp.
    ///
    /// `backupTo` receives a verbatim copy of the previous contents before
    /// anything is written. It must **not** be a path beside the live file:
    /// `Locate` matches `TheNewGentleman*.ini` and takes the newest, so a backup
    /// left in `SKSE/Plugins` would become the file the next export reads. The
    /// snapshot's own `system/tng/` is where it goes.
    ///
    /// Written entries take effect when TNG next *reads* the file, which is at
    /// game start. TNG also rewrites the file from its own in-memory model when
    /// its settings change, so an edit made mid-session can be overwritten before
    /// it is ever read; the caller says so in the report rather than this pretending
    /// otherwise.
    [[nodiscard]] static MergeResult Merge(const std::vector<Upsert>& upserts,
                                           const std::filesystem::path& backupTo);

    /// The character-name field of a player entry key (`Name|RaceFormKey|Sex`), or
    /// the whole key when it has no `|` - which is the honest answer for a shape we
    /// do not recognise, and still usable for the name comparison it is wanted for.
    [[nodiscard]] static std::string PlayerKeyName(std::string_view entryKey);

    /// The sex field of a player entry key, or empty. Read off TNG's own line
    /// rather than derived, because `M`/`F` is TNG's spelling to choose and it is
    /// only guessed when TNG has written nothing to copy.
    [[nodiscard]] static std::string PlayerKeySex(std::string_view entryKey);

    [[nodiscard]] static std::string BuildPlayerKey(std::string_view characterName,
                                                    std::string_view raceFormKey,
                                                    std::string_view sex);
};

}  // namespace SaveMigration::Store
