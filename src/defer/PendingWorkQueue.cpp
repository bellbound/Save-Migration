#include "defer/PendingWorkQueue.h"

#include <algorithm>

namespace SaveMigration::Defer {

PendingWorkQueue& PendingWorkQueue::Get() {
    static PendingWorkQueue instance;
    return instance;
}

bool PendingWorkQueue::Enqueue(PendingItem item) {
    if (item.payload.size() > kMaxPayloadLength) {
        spdlog::error("PendingWorkQueue: payload for '{}'/'{}' is {} bytes, over the {} bound",
                      item.categoryId, item.subjectFormKey, item.payload.size(), kMaxPayloadLength);
        return false;
    }

    std::lock_guard lock(m_mutex);
    if (m_items.size() >= kMaxPendingItems) {
        spdlog::error("PendingWorkQueue: full at {} items, rejecting '{}'/'{}'", kMaxPendingItems,
                      item.categoryId, item.subjectFormKey);
        return false;
    }

    // Same category + same subject twice is a re-queue, not a second job.
    // Keeping both would double-apply on the next trigger.
    for (auto& existing : m_items) {
        if (existing.categoryId == item.categoryId &&
            existing.subjectFormKey == item.subjectFormKey) {
            existing = std::move(item);
            ++m_generation;
            RebuildWatchSets();
            return true;
        }
    }

    m_items.push_back(std::move(item));
    ++m_generation;
    RebuildWatchSets();
    return true;
}

bool PendingWorkQueue::Empty() const {
    std::lock_guard lock(m_mutex);
    return m_items.empty();
}

size_t PendingWorkQueue::Size() const {
    std::lock_guard lock(m_mutex);
    return m_items.size();
}

std::vector<PendingItem> PendingWorkQueue::Items() const {
    std::lock_guard lock(m_mutex);
    return m_items;
}

std::vector<PendingItem> PendingWorkQueue::Items(uint64_t& generationOut) const {
    std::lock_guard lock(m_mutex);
    generationOut = m_generation;
    return m_items;
}

void PendingWorkQueue::Replace(std::vector<PendingItem> items) {
    std::lock_guard lock(m_mutex);
    m_items = std::move(items);
    ++m_generation;
    RebuildWatchSets();
}

void PendingWorkQueue::CommitDrain(
    std::vector<PendingItem> survivors,
    const std::vector<std::pair<std::string, std::string>>& processed,
    uint64_t generationAtStart) {
    std::lock_guard lock(m_mutex);

    if (m_generation == generationAtStart) {
        // Nothing touched the queue during the pass, so `survivors` is the whole
        // truth and the reconciliation below has nothing to find. This is the
        // path taken on essentially every drain.
        m_items = std::move(survivors);
        ++m_generation;
        RebuildWatchSets();
        return;
    }

    // Anything in the live queue that the drain neither saw nor produced was
    // enqueued by an applier during the pass. Wholesale replacement would throw
    // it away - it is not in `survivors`, because `survivors` was built from a
    // copy taken before the pass began.
    const auto wasProcessed = [&processed](const PendingItem& item) {
        for (const auto& [categoryId, subjectKey] : processed) {
            if (item.categoryId == categoryId && item.subjectFormKey == subjectKey) {
                return true;
            }
        }
        return false;
    };
    const auto alreadyKept = [&survivors](const PendingItem& item) {
        for (const auto& kept : survivors) {
            if (kept.categoryId == item.categoryId &&
                kept.subjectFormKey == item.subjectFormKey) {
                return true;
            }
        }
        return false;
    };

    uint32_t adopted = 0;
    for (auto& live : m_items) {
        if (wasProcessed(live) || alreadyKept(live)) {
            continue;
        }
        if (survivors.size() >= kMaxPendingItems) {
            spdlog::error("PendingWorkQueue: full at {} items; dropping '{}'/'{}' enqueued during "
                          "the drain",
                          kMaxPendingItems, live.categoryId, live.subjectFormKey);
            break;
        }
        survivors.push_back(std::move(live));
        ++adopted;
    }
    if (adopted > 0) {
        spdlog::debug("PendingWorkQueue: carried {} item(s) enqueued during the drain", adopted);
    }

    m_items = std::move(survivors);
    ++m_generation;
    RebuildWatchSets();
}

void PendingWorkQueue::RebuildWatchSets() {
    // Caller holds m_mutex.
    m_watchedSubjects.clear();
    m_watchedCells.clear();
    for (const auto& item : m_items) {
        if (!item.subjectFormKey.empty()) {
            m_watchedSubjects.insert(item.subjectFormKey);
        }
        if (!item.cellFormKey.empty()) {
            m_watchedCells.insert(item.cellFormKey);
        }
    }
}

bool PendingWorkQueue::HasWatchedSubjects() const {
    std::lock_guard lock(m_mutex);
    return !m_watchedSubjects.empty();
}

bool PendingWorkQueue::HasWatchedCells() const {
    std::lock_guard lock(m_mutex);
    return !m_watchedCells.empty();
}

bool PendingWorkQueue::IsWatchedSubject(const std::string& key) const {
    std::lock_guard lock(m_mutex);
    return m_watchedSubjects.contains(key);
}

bool PendingWorkQueue::IsWatchedCell(const std::string& key) const {
    std::lock_guard lock(m_mutex);
    return m_watchedCells.contains(key);
}

void PendingWorkQueue::Clear() {
    std::lock_guard lock(m_mutex);
    m_items.clear();
    m_watchedSubjects.clear();
    m_watchedCells.clear();
    ++m_generation;
}

uint64_t PendingWorkQueue::Generation() const {
    std::lock_guard lock(m_mutex);
    return m_generation;
}

void PendingWorkQueue::Save(SKSE::SerializationInterface* intfc) {
    std::lock_guard lock(m_mutex);

    const auto count = static_cast<uint32_t>(std::min<size_t>(m_items.size(), kMaxPendingItems));
    if (!intfc->WriteRecordData(&count, sizeof(count))) {
        spdlog::error("PendingWorkQueue: failed to write item count");
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const auto& item = m_items[i];
        if (!Core::SerializationHub::WriteString(intfc, item.categoryId) ||
            !Core::SerializationHub::WriteString(intfc, item.subjectFormKey) ||
            !Core::SerializationHub::WriteString(intfc, item.cellFormKey) ||
            !Core::SerializationHub::WriteString(intfc, item.worldspaceFormKey) ||
            !intfc->WriteRecordData(&item.trigger, sizeof(item.trigger)) ||
            !intfc->WriteRecordData(&item.attempts, sizeof(item.attempts)) ||
            !intfc->WriteRecordData(&item.maxAttempts, sizeof(item.maxAttempts)) ||
            !intfc->WriteRecordData(&item.expiresAtGameTime, sizeof(item.expiresAtGameTime)) ||
            !Core::SerializationHub::WriteString(intfc, item.payload)) {
            spdlog::error("PendingWorkQueue: write failed at item {}", i);
            return;
        }
    }
    spdlog::debug("PendingWorkQueue: wrote {} pending item(s)", count);
}

bool PendingWorkQueue::Load(SKSE::SerializationInterface* intfc, uint32_t, uint32_t) {
    std::lock_guard lock(m_mutex);
    m_items.clear();

    uint32_t count = 0;
    if (!intfc->ReadRecordData(&count, sizeof(count))) {
        return false;
    }
    if (count > kMaxPendingItems) {
        spdlog::error("PendingWorkQueue: co-save claims {} items, over the {} bound - clearing",
                      count, kMaxPendingItems);
        return false;
    }

    m_items.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        PendingItem item;
        if (!Core::SerializationHub::ReadString(intfc, item.categoryId, 128) ||
            !Core::SerializationHub::ReadString(intfc, item.subjectFormKey, 256) ||
            !Core::SerializationHub::ReadString(intfc, item.cellFormKey, 256) ||
            !Core::SerializationHub::ReadString(intfc, item.worldspaceFormKey, 256) ||
            !intfc->ReadRecordData(&item.trigger, sizeof(item.trigger)) ||
            !intfc->ReadRecordData(&item.attempts, sizeof(item.attempts)) ||
            !intfc->ReadRecordData(&item.maxAttempts, sizeof(item.maxAttempts)) ||
            !intfc->ReadRecordData(&item.expiresAtGameTime, sizeof(item.expiresAtGameTime)) ||
            !Core::SerializationHub::ReadString(intfc, item.payload, kMaxPayloadLength)) {
            // Partial read: discard everything rather than replay a truncated
            // payload against a live actor.
            spdlog::error("PendingWorkQueue: read failed at item {} - clearing queue", i);
            m_items.clear();
            return false;
        }
        m_items.push_back(std::move(item));
    }

    ++m_generation;
    RebuildWatchSets();
    spdlog::info("PendingWorkQueue: restored {} pending item(s)", m_items.size());
    return true;
}

void PendingWorkQueue::Revert() { Clear(); }

}  // namespace SaveMigration::Defer
