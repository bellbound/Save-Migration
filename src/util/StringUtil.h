#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace SaveMigration::Util {

/// True if `text` is a well-formed UTF-8 byte sequence.
bool IsValidUtf8(std::string_view text);

/// Convert text coming out of the Skyrim engine to valid UTF-8.
///
/// Engine text is *usually* the system code page (Windows-1252 for western
/// installs), but plugin authors also ship genuine UTF-8. So: if the bytes
/// already validate as UTF-8, pass them through untouched; otherwise transcode
/// from Windows-1252.
///
/// Windows-1252 is deliberately transcoded rather than treated as Latin-1:
/// 0x80-0x9F is where the em-dash and the curly quotes live, and those are
/// exactly the characters that appear in custom item names.
std::string ConvertSkyrimTextToUTF8(std::string_view text);

/// `nlohmann::json::dump()` throws `type_error.316` on any string that is not
/// valid UTF-8, which aborts a whole report over one oddly-named sword.
/// Try a plain dump; on failure transcode every string in the tree and retry
/// with the replacing error handler as a final net.
std::string SafeDump(const nlohmann::json& json, int indent = -1, char indentChar = ' ',
                     bool ensureAscii = false);

/// `path.string()` throws on paths that don't fit the narrow encoding. Always
/// route paths destined for logs or JSON through here.
std::string PathToUtf8String(const std::filesystem::path& p);

/// Reduce an arbitrary display name to something safe for a directory name.
/// Keeps alphanumerics, space, dash and underscore; collapses everything else
/// to '_'. Empty input yields "Unnamed".
std::string SanitizeForFileName(std::string_view text, size_t maxLength = 48);

std::string Trim(std::string_view text);

/// Split on `delim`, trimming each piece and dropping empties. Used for the
/// comma-separated INI list options (sDisabledCategories, sPluginAliases).
std::vector<std::string> SplitAndTrim(std::string_view text, char delim = ',');

/// Case-insensitive equality for plugin names, script names and editor IDs —
/// all of which the engine treats case-insensitively.
bool IEquals(std::string_view a, std::string_view b);

}  // namespace SaveMigration::Util
