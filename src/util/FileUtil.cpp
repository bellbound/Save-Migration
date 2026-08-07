#include "util/FileUtil.h"

#include <algorithm>

#include <Windows.h>

#include <fstream>

#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Util {

std::filesystem::path DataFolder() {
    char buffer[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    return fs::path(buffer).parent_path() / "Data";
}

bool EnsureDirectory(const fs::path& dir) {
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        return true;
    }
    fs::create_directories(dir, ec);
    if (ec) {
        spdlog::error("FileUtil: create_directories('{}') failed: {}", PathToUtf8String(dir),
                      ec.message());
        return false;
    }
    return true;
}

bool WriteFileAtomic(const fs::path& path, std::string_view content) {
    if (!EnsureDirectory(path.parent_path())) {
        return false;
    }

    fs::path tmp = path;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            spdlog::error("FileUtil: cannot open '{}' for write", PathToUtf8String(tmp));
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            spdlog::error("FileUtil: write to '{}' failed", PathToUtf8String(tmp));
            return false;
        }
    }

    std::error_code ec;
    fs::remove(path, ec);  // rename onto an existing file fails on Windows
    fs::rename(tmp, path, ec);
    if (ec) {
        spdlog::error("FileUtil: rename '{}' -> '{}' failed: {}", PathToUtf8String(tmp),
                      PathToUtf8String(path), ec.message());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool ReadFileToString(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool CopyDirectoryCapped(const fs::path& from, const fs::path& to, uint64_t maxBytes,
                         uint64_t& bytesCopied) {
    bytesCopied = 0;
    std::error_code ec;
    if (!fs::exists(from, ec)) {
        return true;  // nothing to copy is not a failure
    }
    if (!EnsureDirectory(to)) {
        return false;
    }

    for (fs::recursive_directory_iterator it(from, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) {
            spdlog::warn("FileUtil: iteration error under '{}': {}", PathToUtf8String(from),
                         ec.message());
            ec.clear();
            continue;
        }
        const auto relative = fs::relative(it->path(), from, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const auto target = to / relative;

        if (it->is_directory(ec)) {
            EnsureDirectory(target);
            continue;
        }

        const auto size = it->file_size(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (bytesCopied + size > maxBytes) {
            spdlog::warn("FileUtil: side-car copy budget of {} bytes exhausted at '{}'", maxBytes,
                         PathToUtf8String(relative));
            return false;
        }

        EnsureDirectory(target.parent_path());
        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            spdlog::warn("FileUtil: copy '{}' failed: {}", PathToUtf8String(it->path()),
                         ec.message());
            ec.clear();
            continue;
        }
        bytesCopied += size;
    }
    return true;
}

uint64_t DirectorySize(const fs::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return 0;
    }
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->is_regular_file(ec)) {
            const auto size = it->file_size(ec);
            if (!ec) {
                total += size;
            }
            ec.clear();
        }
    }
    return total;
}

void RemoveAllQuiet(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) {
        spdlog::warn("FileUtil: remove_all('{}') failed: {}", PathToUtf8String(dir), ec.message());
    }
}

std::vector<fs::path> ListSubdirectories(const fs::path& dir) {
    std::vector<fs::path> result;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return result;
    }
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it->is_directory(ec)) {
            result.push_back(it->path());
        }
        ec.clear();
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool MovePath(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) {
        return true;
    }

    // Cross-volume rename: fall back to copy then delete.
    ec.clear();
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        spdlog::error("FileUtil: move '{}' -> '{}' failed: {}", PathToUtf8String(from),
                      PathToUtf8String(to), ec.message());
        return false;
    }
    RemoveAllQuiet(from);
    return true;
}

}  // namespace SaveMigration::Util
