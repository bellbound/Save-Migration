#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SaveMigration::Util {

/// Path to the game's `Data` folder, derived from the running executable.
/// Under MO2 this resolves through the VFS to the virtual Data tree.
std::filesystem::path DataFolder();

bool EnsureDirectory(const std::filesystem::path& dir);

/// Write `content` to `path` atomically: `<path>.tmp` then rename over the
/// target. A half-written manifest is worse than a missing one, because the
/// missing case is detected and the half-written case may parse.
bool WriteFileAtomic(const std::filesystem::path& path, std::string_view content);

bool ReadFileToString(const std::filesystem::path& path, std::string& out);

/// Recursive copy that gives up once `maxBytes` have been written, so a
/// runaway side-car (SkyrimNet's prompt archive can reach gigabytes) cannot
/// fill the disk. Returns false if the budget was exhausted; `bytesCopied`
/// always reports what actually landed.
bool CopyDirectoryCapped(const std::filesystem::path& from, const std::filesystem::path& to,
                         uint64_t maxBytes, uint64_t& bytesCopied);

uint64_t DirectorySize(const std::filesystem::path& dir);

/// Delete a tree, swallowing and logging errors. Used for `.staging` and the
/// second rotated generation; never called on anything a user hand-placed.
void RemoveAllQuiet(const std::filesystem::path& dir);

/// Immediate subdirectories of `dir`, sorted by name. Empty if `dir` is absent.
std::vector<std::filesystem::path> ListSubdirectories(const std::filesystem::path& dir);

/// Move `from` onto `to`, falling back to copy+delete when the two live on
/// different volumes (rename across volumes fails on Windows).
bool MovePath(const std::filesystem::path& from, const std::filesystem::path& to);

}  // namespace SaveMigration::Util
