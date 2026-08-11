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

/// True when `relative` is safe to join onto a root and stay inside it.
///
/// False for an absolute path, anything carrying a drive or root name, and
/// anything with a `..` component. This exists because it was not merely
/// theoretical: the side-car copiers derived their in-snapshot keys with
/// `fs::relative(file, DataFolder())`, and under Mod Organizer that returned
/// `../../../../skyrim/MGO4/overwrite/...` for every file that really lived in
/// the overwrite folder. Joined onto the snapshot's `system/` directory, those
/// keys walked back out of the snapshot entirely and wrote fifteen VR Editor
/// files into `%LOCALAPPDATA%/SaveMigration/skyrim/`. The keys are now built
/// from a known root plus a file name rather than by subtraction, and this is
/// the assertion that the construction held.
bool IsContainedRelativePath(const std::filesystem::path& relative);

/// Where a path really points, with Mod Organizer's file system seen through.
/// Empty on any failure, which callers must read as "unknown" rather than
/// substituting the path they asked about.
///
/// usvfs redirects inside `NtCreateFile`, so by the time we hold a handle the
/// kernel already has the real file open and will say so. That makes this the only
/// way to answer two questions that matter for a migration: which *mod* a file
/// came from, and - after writing one - whether it landed in `overwrite` or in
/// some mod's own folder.
std::filesystem::path RealPathOf(const std::filesystem::path& path);

/// True when `realPath` has an `overwrite` component, i.e. Mod Organizer's own
/// folder for writes no mod claims.
///
/// Matched as a whole path component rather than a substring, so a mod whose name
/// contains "overwrite" is not mistaken for it. The folder name is configurable in
/// `ModOrganizer.ini` and in practice never changed; a rename makes this answer
/// "no", which every caller has to be able to survive - none of them may treat a
/// false as proof of anything.
bool IsUnderOverwrite(const std::filesystem::path& realPath);

}  // namespace SaveMigration::Util
