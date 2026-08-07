#include "model/StandingStoneTable.h"

#include <algorithm>
#include <sstream>

#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace SaveMigration::Model {

namespace {

std::filesystem::path TablePath() {
    return Store::SnapshotPaths::Root() / "standing_stones.txt";
}

constexpr std::string_view kTemplate =
    "# Save Migration - standing stone labels\n"
    "#\n"
    "# LABELLING ONLY. Restoring a standing stone re-adds whatever ability form the\n"
    "# snapshot recorded, so this file never affects what is applied - only how it is\n"
    "# named in the report, and which abilities are cleared first so two stones do not\n"
    "# end up active at once.\n"
    "#\n"
    "# One entry per line:\n"
    "#     <FormKey> = <Label>\n"
    "#\n"
    "# A FormKey is \"0x<LocalFormID in hex>~<Plugin filename>\". To find the ability\n"
    "# a doomstone grants, stand at the stone, activate it, then run in the console:\n"
    "#     player.showinventory   (no)  -  instead use:\n"
    "#     help \"Stone\" 0 SPEL\n"
    "# and read the form id of the ability spell. Strip the leading load-order byte\n"
    "# to get the local id.\n"
    "#\n"
    "# Deliberately shipped empty: this project does not hardcode guessed FormIDs.\n"
    "# Example (verify before trusting it):\n"
    "#     0x654D6~Skyrim.esm = The Lord Stone\n";

}  // namespace

StandingStoneTable& StandingStoneTable::Get() {
    static StandingStoneTable instance;
    return instance;
}

void StandingStoneTable::WriteTemplateIfAbsent() {
    const auto path = TablePath();
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return;
    }
    if (Util::WriteFileAtomic(path, kTemplate)) {
        spdlog::info("StandingStoneTable: wrote a template to '{}'", Util::PathToUtf8String(path));
    }
}

void StandingStoneTable::Load() {
    if (m_loaded) {
        return;
    }
    m_loaded = true;
    WriteTemplateIfAbsent();

    std::string raw;
    if (!Util::ReadFileToString(TablePath(), raw)) {
        spdlog::info("StandingStoneTable: no table present; standing stones will not be labelled");
        return;
    }

    std::istringstream stream(raw);
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        const auto trimmed = Util::Trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            spdlog::warn("StandingStoneTable: line {} has no '='; ignoring", lineNumber);
            continue;
        }
        auto key = Util::Trim(std::string_view(trimmed).substr(0, eq));
        auto label = Util::Trim(std::string_view(trimmed).substr(eq + 1));
        if (key.empty() || label.empty()) {
            continue;
        }
        m_entries.emplace(std::move(key), std::move(label));
    }
    spdlog::info("StandingStoneTable: loaded {} label(s)", m_entries.size());
}

std::string StandingStoneTable::Lookup(std::string_view formKey) const {
    const auto it = m_entries.find(std::string(formKey));
    return it == m_entries.end() ? std::string{} : it->second;
}

std::vector<std::string> StandingStoneTable::AllKeys() const {
    std::vector<std::string> keys;
    keys.reserve(m_entries.size());
    for (const auto& [key, label] : m_entries) {
        keys.push_back(key);
    }
    return keys;
}

}  // namespace SaveMigration::Model
