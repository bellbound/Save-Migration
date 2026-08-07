#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SaveMigration::Model {

/// FormKey -> label for the doomstone abilities.
///
/// **Labelling only.** The restore re-adds whatever ability form the snapshot
/// recorded, so an incomplete or wrong table costs a nice name in the report and
/// nothing else. Nothing depends on it being right.
///
/// The table is loaded from a shipped, user-editable file rather than compiled in,
/// precisely so that no standing-stone FormIDs are guessed in source. Add a line
/// and the report starts naming your stone.
///
/// File: `Data/SKSE/Plugins/SaveMigration/standing_stones.txt`
/// Format, one per line, `#` comments allowed:
///     0x654D6~Skyrim.esm = The Lord Stone
class StandingStoneTable {
public:
    static StandingStoneTable& Get();

    /// Read the file if present. Missing file is fine - the feature degrades to
    /// "abilities are not labelled".
    void Load();

    /// Label for a FormKey, or empty if not in the table.
    [[nodiscard]] std::string Lookup(std::string_view formKey) const;

    /// Every key, so the applier can clear whatever stone is currently granted
    /// before applying the recorded one.
    [[nodiscard]] std::vector<std::string> AllKeys() const;

    [[nodiscard]] bool IsLoaded() const { return m_loaded; }
    [[nodiscard]] size_t Size() const { return m_entries.size(); }

    /// Write a commented template so the user has something to edit.
    void WriteTemplateIfAbsent();

private:
    StandingStoneTable() = default;

    bool m_loaded = false;
    std::unordered_map<std::string, std::string> m_entries;
};

}  // namespace SaveMigration::Model
