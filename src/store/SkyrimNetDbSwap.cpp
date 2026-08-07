#include "store/SkyrimNetDbSwap.h"

#include <chrono>
#include <format>
#include <nlohmann/json.hpp>

#include "store/SnapshotPaths.h"
#include "util/FileUtil.h"
#include "util/StringUtil.h"

namespace fs = std::filesystem;

namespace SaveMigration::Store {

fs::path SkyrimNetDbSwap::SkyrimNetDataRoot() {
    return Util::DataFolder() / "SKSE" / "Plugins" / "SkyrimNet";
}

fs::path SkyrimNetDbSwap::LiveDbPath(std::string_view saveId) {
    return SkyrimNetDataRoot() / "data" / std::format("SkyrimNet-{}.db", saveId);
}

bool SkyrimNetDbSwap::WriteMarker(const PendingSwap& swap) {
    const nlohmann::json marker{
        {"targetDbPath", swap.targetDbPath},
        {"pendingDbPath", swap.pendingDbPath},
        {"newSaveId", swap.newSaveId},
        {"oldSaveId", swap.oldSaveId},
        {"preparedAtUnixMs", swap.preparedAtUnixMs},
    };
    const bool ok = Util::WriteFileAtomic(SnapshotPaths::PendingDbMarker(), Util::SafeDump(marker, 2));
    if (ok) {
        spdlog::info("SkyrimNetDbSwap: marker written; the swap happens at the next pre-load");
    } else {
        spdlog::error("SkyrimNetDbSwap: could not write the marker; the swap will not happen");
    }
    return ok;
}

bool SkyrimNetDbSwap::ReadMarker(PendingSwap& out) {
    std::string raw;
    if (!Util::ReadFileToString(SnapshotPaths::PendingDbMarker(), raw)) {
        return false;
    }
    const auto marker = nlohmann::json::parse(raw, nullptr, false);
    if (marker.is_discarded() || !marker.is_object()) {
        spdlog::error("SkyrimNetDbSwap: marker is not valid JSON; discarding it");
        ClearMarker();
        return false;
    }
    out.targetDbPath = marker.value("targetDbPath", "");
    out.pendingDbPath = marker.value("pendingDbPath", "");
    out.newSaveId = marker.value("newSaveId", "");
    out.oldSaveId = marker.value("oldSaveId", "");
    out.preparedAtUnixMs = marker.value("preparedAtUnixMs", int64_t{0});
    return !out.targetDbPath.empty() && !out.pendingDbPath.empty();
}

void SkyrimNetDbSwap::ClearMarker() {
    std::error_code ec;
    fs::remove(SnapshotPaths::PendingDbMarker(), ec);
}

void SkyrimNetDbSwap::ApplyPendingSwap() {
    PendingSwap swap;
    if (!ReadMarker(swap)) {
        return;  // nothing pending: the overwhelmingly common case
    }

    const fs::path pending(swap.pendingDbPath);
    const fs::path target(swap.targetDbPath);

    std::error_code ec;
    if (!fs::exists(pending, ec)) {
        spdlog::error("SkyrimNetDbSwap: marker points at '{}', which is gone; clearing the marker",
                      swap.pendingDbPath);
        ClearMarker();
        return;
    }

    // Back up whatever is there first. This file is the user's rollback if the
    // repaired database turns out worse than the empty one.
    if (fs::exists(target, ec)) {
        auto backup = target;
        backup += ".premigration";
        fs::remove(backup, ec);
        fs::rename(target, backup, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                spdlog::error("SkyrimNetDbSwap: could not back up '{}': {}. Refusing to swap.",
                              swap.targetDbPath, ec.message());
                return;
            }
            fs::remove(target, ec);
        }
        spdlog::info("SkyrimNetDbSwap: existing database backed up to '{}'",
                     Util::PathToUtf8String(backup));
    }
    ec.clear();

    Util::EnsureDirectory(target.parent_path());
    fs::rename(pending, target, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(pending, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            spdlog::error("SkyrimNetDbSwap: swap failed: {}", ec.message());
            return;
        }
        fs::remove(pending, ec);
    }

    ClearMarker();
    spdlog::info("SkyrimNetDbSwap: '{}' is now in place for save {} (migrated from {})",
                 Util::PathToUtf8String(target), swap.newSaveId, swap.oldSaveId);
}

}  // namespace SaveMigration::Store
