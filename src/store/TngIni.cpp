#include "store/TngIni.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <system_error>
#include <vector>

#include "model/FormKeyUtil.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

namespace {

/// Where TNG keeps its settings, relative to `Data`.
constexpr std::string_view kPluginsDir = "SKSE/Plugins";
/// Matched as a prefix, not an exact name: the live file is
/// `TheNewGentleman5.ini` and that 5 is a format generation the mod has already
/// bumped once. Picking the newest match survives the next bump.
constexpr std::string_view kFileStem = "TheNewGentleman";

/// Section-name prefixes, matched case-insensitively. Declared on the class so
/// the import side names the same sections the capture side does.
constexpr std::string_view kPlayerPrefix = TngIni::kPlayerSectionPrefix;
constexpr std::string_view kNpcAddonPrefix = TngIni::kNpcAddonSection;
constexpr std::string_view kNpcSizePrefix = TngIni::kNpcSizeSection;
/// The by-reference counterparts. Absent on the install this was written
/// against, so they are captured on sight rather than assumed to exist - and
/// flagged, because a reference id is only meaningful inside one save.
constexpr std::string_view kRefAddonPrefix = "ActorGenitalAddon";
constexpr std::string_view kRefSizePrefix = "ActorGenitalSize";

std::string_view TrimView(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && Util::IEquals(text.substr(0, prefix.size()), prefix);
}

bool IsHexRun(std::string_view text, size_t length) {
    if (text.size() != length) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

std::optional<int32_t> ParseInt(std::string_view text) {
    const auto trimmed = TrimView(text);
    int32_t value = 0;
    const auto result =
        std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
    if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size()) {
        return std::nullopt;
    }
    return value;
}

/// Shape-only. Deliberately does **not** resolve: this runs on the worker, where
/// touching `TESDataHandler` is not allowed, and a key that names an absent
/// plugin is still worth carrying so the import can say what was lost.
bool LooksLikeFormKey(std::string_view text) {
    return Model::FormKeyUtil::ParseFormKey(TrimView(text)).has_value();
}

/// Split on `|`, keeping empty fields, because position carries meaning.
std::vector<std::string_view> SplitPipes(std::string_view text) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (;;) {
        const auto bar = text.find('|', start);
        if (bar == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, bar - start));
        start = bar + 1;
    }
    return parts;
}

/// The located file, read with the byte bound applied.
///
/// Shared by both directions so the bound, the "absence is not a failure" rule
/// and the relative name in the report cannot drift between them.
struct LoadedIni {
    /// True when nothing went wrong. A missing file sets this *and* leaves
    /// `found` false: TNG writes its settings lazily, so absence means it has had
    /// no reason to yet, not that anything is broken.
    bool ok = false;
    bool found = false;
    std::string error;
    /// Relative to `Data`, for reports.
    std::string usedFile;
    fs::path path;
    uint64_t bytes = 0;
    std::string text;
};

LoadedIni LoadIni() {
    LoadedIni out;
    out.path = TngIni::Locate();
    if (out.path.empty()) {
        out.ok = true;
        return out;
    }

    out.found = true;
    out.usedFile = std::format("{}/{}", kPluginsDir, Util::PathToUtf8String(out.path.filename()));

    std::error_code ec;
    out.bytes = fs::file_size(out.path, ec);
    if (ec) {
        out.error = std::format("could not size '{}'", out.usedFile);
        return out;
    }
    if (out.bytes > TngIni::kMaxIniBytes) {
        // Refuse rather than read. Nothing legitimate reaches this size, and the
        // alternative is a multi-megabyte allocation on the strength of another
        // mod's file.
        out.error = std::format("'{}' is {} bytes, over the {} byte bound", out.usedFile, out.bytes,
                                TngIni::kMaxIniBytes);
        return out;
    }
    if (!Util::ReadFileToString(out.path, out.text)) {
        out.error = std::format("could not read '{}'", out.usedFile);
        return out;
    }

    out.ok = true;
    return out;
}

struct RawEntry {
    std::string key;
    std::string value;
};

struct RawSection {
    std::string name;
    std::vector<RawEntry> entries;
    /// Set when the section hit `kMaxEntriesPerSection`.
    bool truncated = false;
};

/// A deliberately forgiving INI reader.
///
/// Not SimpleIni, even though it is already in the build, for two reasons: the
/// values here contain `~`, `|`, spaces and dots and must survive byte-for-byte,
/// and unknown sections have to be *observable* rather than normalised away.
/// Splitting on the first `=` only is what keeps a value containing `=` intact.
std::vector<RawSection> ReadSections(std::string_view text) {
    std::vector<RawSection> sections;
    RawSection current;
    current.name = "";  // entries before any header, if a future format has them

    size_t pos = 0;
    while (pos <= text.size()) {
        const auto newline = text.find('\n', pos);
        const auto line = TrimView(
            text.substr(pos, newline == std::string_view::npos ? std::string_view::npos
                                                              : newline - pos));
        pos = newline == std::string_view::npos ? text.size() + 1 : newline + 1;

        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close == std::string_view::npos) {
                continue;  // malformed header: skip it rather than mis-attribute
            }
            if (!current.entries.empty() || !current.name.empty()) {
                sections.push_back(std::move(current));
            }
            current = RawSection{};
            current.name = std::string(TrimView(line.substr(1, close - 1)));
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;  // not a key/value line in any format we can use
        }
        if (current.entries.size() >= TngIni::kMaxEntriesPerSection) {
            current.truncated = true;
            continue;
        }
        current.entries.push_back(RawEntry{std::string(TrimView(line.substr(0, equals))),
                                           std::string(TrimView(line.substr(equals + 1)))});
    }
    if (!current.entries.empty() || !current.name.empty()) {
        sections.push_back(std::move(current));
    }
    return sections;
}

/// `Name|BaseFormKey|Sex = AddonFormKey|SizeCategory`
nlohmann::json ParsePlayerEntry(const RawEntry& entry, bool& parsed) {
    nlohmann::json out{{"rawKey", entry.key}, {"rawValue", entry.value}};
    parsed = false;

    const auto keyParts = SplitPipes(entry.key);
    const auto valueParts = SplitPipes(entry.value);
    if (keyParts.size() < 3 || valueParts.empty()) {
        out["parsed"] = false;
        out["parseNote"] = "expected 'name|baseFormKey|sex = addonFormKey|size'";
        return out;
    }

    out["characterName"] = std::string(TrimView(keyParts[0]));
    out["baseFormKey"] = std::string(TrimView(keyParts[1]));
    out["sex"] = std::string(TrimView(keyParts[2]));
    out["addonFormKey"] = std::string(TrimView(valueParts[0]));
    out["baseFormKeyLooksValid"] = LooksLikeFormKey(keyParts[1]);
    // An empty addon is a real answer - "this character has none" - so it is not
    // treated as a parse failure.
    out["addonFormKeyLooksValid"] =
        TrimView(valueParts[0]).empty() ? true : LooksLikeFormKey(valueParts[0]);

    if (valueParts.size() >= 2) {
        if (const auto size = ParseInt(valueParts[1])) {
            out["sizeCategory"] = *size;
        } else {
            out["sizeCategoryRaw"] = std::string(TrimView(valueParts[1]));
        }
    }
    // Anything beyond the fields we know about is kept rather than discarded: a
    // future TNG that appends a field should show up in the export, not vanish.
    if (keyParts.size() > 3 || valueParts.size() > 2) {
        auto extra = nlohmann::json::array();
        for (size_t i = 3; i < keyParts.size(); ++i) {
            extra.push_back(std::string(TrimView(keyParts[i])));
        }
        for (size_t i = 2; i < valueParts.size(); ++i) {
            extra.push_back(std::string(TrimView(valueParts[i])));
        }
        out["extraFields"] = std::move(extra);
    }

    parsed = true;
    out["parsed"] = true;
    return out;
}

/// `FormKey = FormKey` (addon) or `FormKey = int` (size).
nlohmann::json ParseNpcEntry(const RawEntry& entry, bool wantsInt, bool& parsed) {
    nlohmann::json out{{"rawKey", entry.key}, {"rawValue", entry.value}};
    parsed = false;

    out["subjectFormKey"] = entry.key;
    out["subjectFormKeyLooksValid"] = LooksLikeFormKey(entry.key);

    if (wantsInt) {
        if (const auto size = ParseInt(entry.value)) {
            out["sizeCategory"] = *size;
            parsed = true;
        } else {
            out["parseNote"] = "expected an integer size category";
        }
    } else {
        out["addonFormKey"] = entry.value;
        const bool ok = entry.value.empty() || LooksLikeFormKey(entry.value);
        out["addonFormKeyLooksValid"] = ok;
        parsed = ok;
        if (!ok) {
            out["parseNote"] = "expected a '0x<local id>~<plugin>' form key";
        }
    }

    out["parsed"] = parsed;
    return out;
}

}  // namespace

fs::path TngIni::Locate() {
    const auto dir = Util::DataFolder() / fs::path(kPluginsDir);
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        return {};
    }

    // Newest wins. The mod bumps a generation digit rather than migrating in
    // place, so on an install that has been through an upgrade both files exist
    // and the older one is stale.
    fs::path best;
    fs::file_time_type bestTime{};
    for (const auto& item : fs::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!item.is_regular_file(ec) || ec) {
            continue;
        }
        const auto name = Util::PathToUtf8String(item.path().filename());
        if (!StartsWithNoCase(name, kFileStem)) {
            continue;
        }
        if (name.size() < 4 || !Util::IEquals(name.substr(name.size() - 4), ".ini")) {
            continue;
        }
        std::error_code timeEc;
        const auto written = fs::last_write_time(item.path(), timeEc);
        if (timeEc) {
            continue;
        }
        if (best.empty() || written > bestTime) {
            best = item.path();
            bestTime = written;
        }
    }
    return best;
}

std::string TngIni::SaveTokenFromPath(std::string_view savePath) {
    // The save id is the second underscore-delimited field of the file name -
    // `Save8_E223B073_0_...` - but rather than trust the position, take the first
    // field that *is* eight hex digits. Autosaves and quicksaves use the same
    // layout with a different leading word, and a name we do not recognise
    // yields an empty token rather than a wrong one.
    auto name = fs::path(std::string(savePath)).filename().string();
    if (name.empty()) {
        name = std::string(savePath);
    }

    size_t start = 0;
    while (start <= name.size()) {
        const auto underscore = name.find('_', start);
        const auto field = std::string_view(name).substr(
            start, underscore == std::string::npos ? std::string::npos : underscore - start);
        if (IsHexRun(field, 8)) {
            return std::string(field);
        }
        if (underscore == std::string::npos) {
            break;
        }
        start = underscore + 1;
    }
    return {};
}

TngIni::Result TngIni::Capture(std::string_view saveToken, std::string_view characterName) {
    Result result;
    result.success = true;  // absence is a successful capture of nothing

    const auto loaded = LoadIni();
    result.found = loaded.found;
    result.usedFile = loaded.usedFile;
    if (!loaded.ok) {
        result.success = false;
        result.error = loaded.error;
        return result;
    }
    if (!loaded.found) {
        result.document = nlohmann::json{
            {"schemaVersion", 1},
            {"iniFound", false},
            {"note", "No TheNewGentleman*.ini was present under Data/SKSE/Plugins, so there was "
                     "nothing to record. This is normal when TNG is not installed."},
        };
        return result;
    }

    const auto bytes = loaded.bytes;
    const auto sections = ReadSections(loaded.text);

    /// Every `[PlayerInfo…]` section, held until all are read so the
    /// character-name fallback can require *exactly one* candidate.
    struct PlayerSection {
        std::string name;
        std::string token;
        bool namesThisCharacter = false;
        nlohmann::json entries;
    };
    std::vector<PlayerSection> playerSections;

    auto playerCurrent = nlohmann::json::array();
    auto playerOther = nlohmann::json::object();
    auto npcAddon = nlohmann::json::array();
    auto npcSize = nlohmann::json::array();
    auto refAddon = nlohmann::json::array();
    auto refSize = nlohmann::json::array();
    auto sectionsSeen = nlohmann::json::array();
    auto sectionsNotCaptured = nlohmann::json::array();
    auto truncated = nlohmann::json::array();
    std::string matchedSection;

    const auto appendNpc = [&result](nlohmann::json& into, const RawSection& section,
                                    bool wantsInt) {
        for (const auto& entry : section.entries) {
            bool parsed = false;
            into.push_back(ParseNpcEntry(entry, wantsInt, parsed));
            ++result.npcEntries;
            if (!parsed) {
                ++result.unparsedEntries;
            }
        }
    };

    for (const auto& section : sections) {
        if (!section.name.empty()) {
            sectionsSeen.push_back(section.name);
        }
        if (section.truncated) {
            truncated.push_back(section.name);
        }

        if (StartsWithNoCase(section.name, kPlayerPrefix)) {
            const auto token = TrimView(std::string_view(section.name).substr(kPlayerPrefix.size()));
            auto entries = nlohmann::json::array();
            bool namesThisCharacter = false;
            for (const auto& entry : section.entries) {
                bool parsed = false;
                auto parsedEntry = ParsePlayerEntry(entry, parsed);
                if (!parsed) {
                    ++result.unparsedEntries;
                } else if (!characterName.empty() &&
                           Util::IEquals(parsedEntry.value("characterName", std::string{}),
                                         characterName)) {
                    namesThisCharacter = true;
                }
                entries.push_back(std::move(parsedEntry));
            }
            // Deciding which section is ours is deferred until every one has been
            // read, because the name fallback below can only be judged once it is
            // known whether exactly one section names this character.
            playerSections.push_back(
                PlayerSection{section.name, std::string(token), namesThisCharacter,
                              std::move(entries)});
            continue;
        }

        if (StartsWithNoCase(section.name, kNpcAddonPrefix)) {
            appendNpc(npcAddon, section, /*wantsInt=*/false);
        } else if (StartsWithNoCase(section.name, kNpcSizePrefix)) {
            appendNpc(npcSize, section, /*wantsInt=*/true);
        } else if (StartsWithNoCase(section.name, kRefAddonPrefix)) {
            appendNpc(refAddon, section, /*wantsInt=*/false);
        } else if (StartsWithNoCase(section.name, kRefSizePrefix)) {
            appendNpc(refSize, section, /*wantsInt=*/true);
        } else if (!section.name.empty()) {
            // Skeletons, hotkeys, revealing-mod flags, log level. Machine and
            // install preferences rather than playthrough data, so named but not
            // taken - and named precisely so that a section that *starts* holding
            // playthrough data is visible in the next export.
            sectionsNotCaptured.push_back(section.name);
        }
    }

    // ── Which player section is this save's ───────────────────────────────
    //
    // The save id is authoritative: TNG builds the section name from it, so an
    // exact match is not a guess. The character name is a fallback for the case
    // the id cannot cover - a hand-renamed save carries no id in its file name at
    // all, and that is what every test save looks like - and it is only accepted
    // when *exactly one* section names this character, because two saves of the
    // same character are common and picking either would be a coin toss.
    //
    // Whatever is not chosen is still recorded, under `playerOtherSaveSections`.
    // Being wrong about which section is ours must cost accuracy, never data.
    size_t chosen = playerSections.size();
    for (size_t i = 0; i < playerSections.size(); ++i) {
        if (!saveToken.empty() && Util::IEquals(playerSections[i].token, saveToken)) {
            chosen = i;
            result.playerMatchedBy = "save id";
            break;
        }
    }
    if (chosen == playerSections.size() && !characterName.empty()) {
        size_t byName = playerSections.size();
        uint32_t candidates = 0;
        for (size_t i = 0; i < playerSections.size(); ++i) {
            if (playerSections[i].namesThisCharacter) {
                byName = i;
                ++candidates;
            }
        }
        if (candidates == 1) {
            chosen = byName;
            result.playerMatchedBy = "character name";
        }
    }

    for (size_t i = 0; i < playerSections.size(); ++i) {
        if (i == chosen) {
            matchedSection = playerSections[i].name;
            result.matchedPlayerSection = true;
            result.playerEntries = static_cast<uint32_t>(playerSections[i].entries.size());
            playerCurrent = std::move(playerSections[i].entries);
        } else {
            playerOther[playerSections[i].name] = std::move(playerSections[i].entries);
        }
    }

    result.document = nlohmann::json{
        {"schemaVersion", 1},
        {"iniFound", true},
        {"sourceFile", result.usedFile},
        {"sourceBytes", bytes},
        {"saveToken", std::string(saveToken)},
        {"characterName", std::string(characterName)},
        {"playerSection", matchedSection},
        {"matchedPlayerSection", result.matchedPlayerSection},
        {"playerMatchedBy", result.playerMatchedBy},
        {"player", std::move(playerCurrent)},
        {"playerOtherSaveSections", std::move(playerOther)},
        {"npcByBase", nlohmann::json{{"addon", std::move(npcAddon)},
                                     {"size", std::move(npcSize)}}},
        {"npcByReference", nlohmann::json{{"addon", std::move(refAddon)},
                                          {"size", std::move(refSize)}}},
        {"sectionsSeen", std::move(sectionsSeen)},
        {"sectionsNotCaptured", std::move(sectionsNotCaptured)},
        {"sectionsTruncated", std::move(truncated)},
        {"unparsedEntries", result.unparsedEntries},
        {"note",
         "Values are recorded as data, not as a copy of the file, so an import can validate each "
         "one against the load order it is going into before writing it. Every entry keeps rawKey "
         "and rawValue verbatim; 'parsed' says whether the shape TNG uses today was recognised. "
         "NPC entries under npcByBase are keyed by base actor record and are save-agnostic, so TNG "
         "shares them across every save - but only across every save of *this modlist*, since the "
         "file is per install. Another modlist has its own, which is why this half is migrated. "
         "Entries under npcByReference are keyed by a reference id, which is private to one save "
         "and is not portable if it is dynamic (0xFF...). The player section is the only part TNG "
         "itself scopes per save."},
    };

    if (!result.matchedPlayerSection) {
        result.document["playerNote"] =
            playerSections.empty()
                ? "TNG has written no player section at all, so there is nothing to single out."
                : "No player section could be singled out: the save's file name carries no "
                  "eight-hex-digit id to match, or TNG has not written one for this save line, and "
                  "the character name did not identify exactly one section either. Every section "
                  "found is under playerOtherSaveSections, so nothing is lost - only the question "
                  "of which one is current is open.";
    } else if (result.playerMatchedBy == "character name") {
        result.document["playerNote"] =
            "Matched by character name, not by save id: this save's file name carries no id to "
            "match on, which is normal for a renamed or hand-made save. Exactly one section named "
            "this character, so it is the only candidate - but it is an inference, not the "
            "authoritative match.";
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// The import direction
// ─────────────────────────────────────────────────────────────────────────────

const TngIni::Section* TngIni::LiveFile::Find(std::string_view section) const {
    for (const auto& candidate : sections) {
        if (Util::IEquals(candidate.name, section)) {
            return &candidate;
        }
    }
    return nullptr;
}

TngIni::LiveFile TngIni::ReadLive() {
    LiveFile out;
    const auto loaded = LoadIni();
    out.found = loaded.found;
    out.usedFile = loaded.usedFile;
    if (!loaded.ok) {
        out.error = loaded.error;
        return out;
    }
    out.success = true;
    if (!loaded.found) {
        return out;
    }

    for (auto& raw : ReadSections(loaded.text)) {
        if (raw.name.empty()) {
            continue;  // entries before any header: no section to address them by
        }
        Section section;
        section.name = std::move(raw.name);
        section.entries.reserve(raw.entries.size());
        for (auto& entry : raw.entries) {
            section.entries.push_back(Entry{std::move(entry.key), std::move(entry.value)});
        }
        out.sections.push_back(std::move(section));
    }
    return out;
}

std::string TngIni::PlayerKeyName(std::string_view entryKey) {
    const auto parts = SplitPipes(entryKey);
    return parts.empty() ? std::string(TrimView(entryKey)) : std::string(TrimView(parts[0]));
}

std::string TngIni::PlayerKeySex(std::string_view entryKey) {
    const auto parts = SplitPipes(entryKey);
    return parts.size() >= 3 ? std::string(TrimView(parts[2])) : std::string{};
}

std::string TngIni::BuildPlayerKey(std::string_view characterName, std::string_view raceFormKey,
                                   std::string_view sex) {
    return std::format("{}|{}|{}", characterName, raceFormKey, sex);
}

namespace {

/// The byte-order mark TNG's file carries. Preserved rather than stripped: this
/// is another mod's file and its own parser is the only opinion that counts.
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

/// One line of the file, held verbatim with just enough about it to find and
/// edit. Nothing here re-serialises a parsed model, which is what would silently
/// normalise away the comments, blank-line rhythm and CRLF endings TNG wrote.
struct EditLine {
    /// Verbatim, without its terminator.
    std::string text;
    /// The section this line sits in. A header line's section is the one it opens,
    /// so "find this section's header" is the same search as "find its entries".
    std::string section;
    bool isHeader = false;
    /// Empty when the line is not `key = value`.
    std::string key;
};

/// What the file looked like, so the rewrite can look the same.
struct TextShape {
    bool bom = false;
    std::string newline = "\r\n";
};

std::vector<EditLine> SplitLines(std::string_view body) {
    std::vector<EditLine> lines;
    std::string currentSection;

    size_t pos = 0;
    for (;;) {
        const auto newline = body.find('\n', pos);
        auto raw = newline == std::string_view::npos ? body.substr(pos)
                                                     : body.substr(pos, newline - pos);
        // The terminator style is recorded once for the whole file rather than
        // carried per line, so the `\r` comes off here.
        if (!raw.empty() && raw.back() == '\r') {
            raw.remove_suffix(1);
        }

        EditLine line;
        line.text = std::string(raw);
        const auto trimmed = TrimView(raw);
        if (!trimmed.empty() && trimmed.front() == '[') {
            if (const auto close = trimmed.find(']'); close != std::string_view::npos) {
                currentSection = std::string(TrimView(trimmed.substr(1, close - 1)));
                line.isHeader = true;
            }
        } else if (!trimmed.empty() && trimmed.front() != ';' && trimmed.front() != '#') {
            if (const auto equals = trimmed.find('='); equals != std::string_view::npos) {
                line.key = std::string(TrimView(trimmed.substr(0, equals)));
            }
        }
        line.section = currentSection;
        lines.push_back(std::move(line));

        if (newline == std::string_view::npos) {
            break;
        }
        pos = newline + 1;
    }
    return lines;
}

/// The value side of a `key = value` line, trimmed.
std::string ValueOf(const EditLine& line) {
    const auto trimmed = TrimView(line.text);
    const auto equals = trimmed.find('=');
    if (equals == std::string_view::npos) {
        return {};
    }
    return std::string(TrimView(trimmed.substr(equals + 1)));
}

/// `key = value`, in TNG's own spacing. The key is taken from the line being
/// replaced where there is one, so TNG's capitalisation survives an edit.
std::string ComposeLine(std::string_view key, std::string_view value) {
    return std::format("{} = {}", key, value);
}

/// The live line for `(section, key)`, or npos. Case-insensitive on both, because
/// that is how TNG's own reader matches and how our capture side matches.
size_t FindLine(const std::vector<EditLine>& lines, std::string_view section,
                std::string_view key) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].key.empty()) {
            continue;
        }
        if (Util::IEquals(lines[i].section, section) && Util::IEquals(lines[i].key, key)) {
            return i;
        }
    }
    return std::string::npos;
}

/// Where a new entry for `section` goes: after its last header-or-entry line.
///
/// Deliberately not "after the last line in the section" - a section's trailing
/// blank lines parse as belonging to it, and inserting after those would put the
/// new key below a blank line, separated from the block it belongs to.
size_t InsertionPointFor(const std::vector<EditLine>& lines, std::string_view section) {
    size_t at = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!Util::IEquals(lines[i].section, section)) {
            continue;
        }
        if (lines[i].isHeader || !lines[i].key.empty()) {
            at = i;
        }
    }
    return at;
}

}  // namespace

TngIni::MergeResult TngIni::Merge(const std::vector<Upsert>& upserts, const fs::path& backupTo) {
    MergeResult result;

    const auto loaded = LoadIni();
    if (!loaded.ok) {
        result.error = loaded.error;
        return result;
    }
    if (!loaded.found) {
        // Deliberately not created. `Locate` picks the newest
        // `TheNewGentleman*.ini`, and the digit in that name is a format
        // generation TNG has already bumped once - so inventing a file name means
        // guessing the generation, and guessing low writes a file TNG will never
        // read while guessing high shadows the one it does. TNG writes this file
        // itself; its absence is a fact to report, not one to paper over.
        result.error = "TNG has written no settings file, so there is nowhere to put these that "
                       "TNG would read";
        return result;
    }

    result.targetFile = loaded.usedFile;

    // Backed up *before* the first byte is written, and into the snapshot rather
    // than beside the live file - see the header for why beside it would be a
    // trap. A settings file is small and the previous contents are the only way
    // back from a merge that turns out wrong.
    if (!backupTo.empty()) {
        if (Util::EnsureDirectory(backupTo.parent_path()) &&
            Util::WriteFileAtomic(backupTo, loaded.text)) {
            result.backupFile = Util::PathToUtf8String(backupTo);
        } else {
            // Not fatal. Losing the backup costs a way back; refusing the import
            // over it costs the import.
            spdlog::warn("TngIni: could not write the pre-import backup to '{}'",
                         Util::PathToUtf8String(backupTo));
        }
    }

    std::string body = loaded.text;
    TextShape shape;
    if (body.starts_with(kUtf8Bom)) {
        shape.bom = true;
        body.erase(0, kUtf8Bom.size());
    }
    // Whatever the file already uses. TNG writes CRLF, which is also the default
    // for the one case with nothing to copy - a file holding a single line.
    shape.newline = body.find("\r\n") == std::string::npos && body.find('\n') != std::string::npos
                        ? "\n"
                        : "\r\n";

    auto lines = SplitLines(body);

    for (const auto& upsert : upserts) {
        const auto at = FindLine(lines, upsert.section, upsert.key);
        if (at != std::string::npos) {
            if (ValueOf(lines[at]) == upsert.value) {
                ++result.identical;
                continue;
            }
            lines[at].text = ComposeLine(lines[at].key, upsert.value);
            ++result.changed;
            continue;
        }

        EditLine fresh;
        fresh.section = upsert.section;
        fresh.key = upsert.key;
        fresh.text = ComposeLine(upsert.key, upsert.value);

        const auto insertAt = InsertionPointFor(lines, upsert.section);
        if (insertAt != std::string::npos) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt) + 1,
                         std::move(fresh));
            ++result.added;
            continue;
        }

        // A section TNG has never written. Appended in TNG's own rhythm: two blank
        // lines, the header, the entries. The trailing newline is taken off and put
        // back so the new block lands inside the file rather than after its end.
        bool hadTrailingNewline = false;
        while (!lines.empty() && lines.back().text.empty() && !lines.back().isHeader &&
               lines.back().key.empty()) {
            lines.pop_back();
            hadTrailingNewline = true;
        }
        for (int i = 0; i < 2; ++i) {
            lines.push_back(EditLine{});
        }
        EditLine header;
        header.section = upsert.section;
        header.isHeader = true;
        header.text = std::format("[{}]", upsert.section);
        lines.push_back(std::move(header));
        lines.push_back(std::move(fresh));
        if (hadTrailingNewline) {
            lines.push_back(EditLine{});
        }
        ++result.added;
        ++result.sectionsCreated;
    }

    if (result.added == 0 && result.changed == 0) {
        // Nothing to say, so nothing is written. Rewriting a file to produce
        // identical bytes still moves its timestamp, and `Locate` picks by
        // timestamp.
        result.success = true;
        return result;
    }

    std::string out;
    out.reserve(body.size() + upserts.size() * 64);
    if (shape.bom) {
        out.append(kUtf8Bom);
    }
    bool first = true;
    for (const auto& line : lines) {
        if (!first) {
            out.append(shape.newline);
        }
        first = false;
        out.append(line.text);
    }

    if (!Util::WriteFileAtomic(loaded.path, out)) {
        result.error = std::format("could not write '{}'", result.targetFile);
        return result;
    }

    result.success = true;
    spdlog::info("TngIni: merged into '{}' - {} added, {} changed, {} already matching, {} "
                 "section(s) created",
                 result.targetFile, result.added, result.changed, result.identical,
                 result.sectionsCreated);
    return result;
}

}  // namespace SaveMigration::Store
