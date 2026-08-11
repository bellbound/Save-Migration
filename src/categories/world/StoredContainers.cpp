#include "categories/world/StoredContainers.h"

#include <format>
#include <map>
#include <vector>

#include "config/MigrationConfig.h"
#include "model/FormRef.h"
#include "util/StringUtil.h"

namespace SaveMigration::Categories {

namespace {

constexpr std::string_view kId = "world.stored_containers";

/// A ceiling on how many containers get written down, so a load order that
/// somehow marks everything as changed cannot turn one harvest into a
/// hundred-megabyte file. Reaching it is reported rather than swallowed.
///
/// Deliberately far above anything real, because it is a backstop and nothing
/// else - the pass-one filter below is what makes this category affordable, and
/// the ceiling only exists so a pathological load order cannot make us allocate
/// without bound. It used to be 4000, which a normal save reached: an install
/// with 16898 container references had 6641 worth reading, so the cap was
/// silently truncating a third of a real export. Nothing about 4000 was a
/// correctness bound.
constexpr size_t kMaxContainers = 50000;

/// The container's default contents, as the level designer left them.
std::map<RE::TESBoundObject*, int32_t> BaseContents(const RE::TESObjectCONT* container) {
    std::map<RE::TESBoundObject*, int32_t> counts;
    if (!container) {
        return counts;
    }
    container->ForEachContainerObject([&counts](RE::ContainerObject& entry) {
        if (entry.obj && entry.count > 0) {
            counts[entry.obj] += entry.count;
        }
        return RE::BSContainer::ForEachResult::kContinue;
    });
    return counts;
}

/// Everything present beyond the base record. Negative differences - the player
/// took the designer's ingots - are deliberately dropped: this category adds,
/// it never removes, because the new game's copy of that chest is the new
/// game's business.
std::map<RE::TESBoundObject*, int32_t> AddedContents(RE::TESObjectREFR* ref,
                                                     const RE::TESObjectCONT* base) {
    std::map<RE::TESBoundObject*, int32_t> added;
    const auto baseCounts = BaseContents(base);
    for (const auto& [object, count] : ref->GetInventoryCounts()) {
        if (!object || count <= 0) {
            continue;
        }
        int32_t baseline = 0;
        if (const auto found = baseCounts.find(object); found != baseCounts.end()) {
            baseline = found->second;
        }
        if (const auto surplus = count - baseline; surplus > 0) {
            added[object] = surplus;
        }
    }
    return added;
}

bool IsDynamic(RE::FormID formId) { return (formId & 0xFF000000u) == 0xFF000000u; }

}  // namespace

const Core::CategoryDescriptor& StoredContainers::Describe() const {
    static const Core::CategoryDescriptor descriptor{
        .id = kId,
        .displayName = "Stored containers",
        // With the world state rather than with inventory: these are references
        // out in the world, and the only ordering constraint is that map markers
        // and cleared locations have settled so the report reads coherently.
        // Nothing here depends on the player, so it deliberately runs before the
        // teleport rather than after.
        .phase = Core::Phase::kWorldState,
        .restoreMode = Core::RestoreMode::kInstant,
        .requirement = {},
        .schemaVersion = 1,
    };
    return descriptor;
}

void StoredContainers::Collect(Core::CollectContext& ctx) {
    const auto subject = Report::WorldSubject("Containers");

    // Pass one: pull candidates out of the form table under its read lock, and
    // do nothing else while holding it. `GetInventoryCounts` allocates and can
    // touch the inventory-changes machinery, which is not something to do with a
    // global lock held.
    std::vector<RE::TESObjectREFR*> candidates;
    uint32_t referencesSeen = 0;
    uint32_t untouched = 0;
    uint32_t respawning = 0;
    {
        const auto& [forms, lock] = RE::TESForm::GetAllForms();
        const RE::BSReadLockGuard guard{lock};
        if (!forms) {
            ctx.report.FailCategory(Report::ReasonCode::kIoError, "the form table is unreachable");
            return;
        }
        for (const auto& [formId, form] : *forms) {
            if (!form || form->GetFormType() != RE::FormType::Reference) {
                continue;
            }
            auto* ref = form->As<RE::TESObjectREFR>();
            if (!ref || IsDynamic(formId) || ref->IsMarkedForDeletion()) {
                continue;
            }
            auto* base = ref->GetBaseObject();
            auto* container = base ? base->As<RE::TESObjectCONT>() : nullptr;
            if (!container) {
                continue;
            }
            ++referencesSeen;

            if (container->data.flags.any(RE::CONT_DATA::Flag::kRespawn)) {
                ++respawning;
                continue;
            }
            // The whole reason this category is affordable. A container nobody
            // has opened and altered shares its base record's list and carries
            // no changes object at all, so this one test removes the great
            // majority of the world.
            if (!ref->extraList.HasType<RE::ExtraContainerChanges>()) {
                ++untouched;
                continue;
            }
            candidates.push_back(ref);
        }
    }

    auto entries = nlohmann::json::array();
    uint32_t stacks = 0;
    uint32_t unnameable = 0;
    bool capped = false;

    for (auto* ref : candidates) {
        if (entries.size() >= kMaxContainers) {
            capped = true;
            break;
        }
        const auto containerKey = Model::FormKeyUtil::BuildFormKey(ref);
        if (containerKey.empty()) {
            continue;  // no cross-save identity for this reference
        }
        auto* base = ref->GetBaseObject();
        const auto added = AddedContents(ref, base ? base->As<RE::TESObjectCONT>() : nullptr);
        if (added.empty()) {
            continue;  // changed at some point, but nothing extra in it now
        }

        auto items = nlohmann::json::array();
        for (const auto& [object, count] : added) {
            const auto itemKey = Model::FormKeyUtil::BuildFormKey(object);
            if (itemKey.empty()) {
                // A crafted or player-enchanted item has a dynamic form id and
                // cannot be named across saves. Counted so the report can say
                // how much of a container did not travel, rather than quietly
                // shrinking it.
                ++unnameable;
                continue;
            }
            const char* itemName = object->GetName();
            items.push_back({
                {"form", itemKey},
                {"name", (itemName && *itemName) ? Util::ConvertSkyrimTextToUTF8(itemName) : ""},
                {"count", count},
            });
            ++stacks;
        }
        if (items.empty()) {
            continue;
        }

        const char* refName = ref->GetName();
        nlohmann::json entry{
            {"form", containerKey},
            {"name", (refName && *refName) ? Util::ConvertSkyrimTextToUTF8(refName) : ""},
            {"items", std::move(items)},
        };
        if (auto* cell = ref->GetParentCell()) {
            entry["cell"] = Model::FormKeyUtil::BuildFormKey(cell);
            const char* cellName = cell->GetName();
            if (cellName && *cellName) {
                entry["cellName"] = Util::ConvertSkyrimTextToUTF8(cellName);
            }
        }
        entries.push_back(std::move(entry));
    }

    auto& payload = ctx.Payload(kId, Describe().schemaVersion);
    const auto recorded = entries.size();
    payload["containers"] = std::move(entries);
    payload["method"] =
        "Every container reference in the form table, filtered to those carrying an "
        "ExtraContainerChanges - the engine allocates that only once something has altered the "
        "contents - and then reduced to the difference from the base record's own item list.";

    ctx.report.Succeeded(subject, "stored_containers", "",
                         std::format("{} container(s), {} stack(s)", recorded, stacks));
    ctx.report.Info(std::format(
        "{} container reference(s) exist; {} had never been altered and {} respawn on a timer, "
        "leaving {} worth reading and {} with anything of the player's still in them.",
        referencesSeen, untouched, respawning, candidates.size(), recorded));
    if (unnameable > 0) {
        ctx.report.Warn(Report::ReasonCode::kPartialByDesign,
                        std::format("{} stored item(s) are crafted or player-enchanted, so they "
                                    "have a dynamic form id and cannot be referenced in another "
                                    "save. They are not recorded.",
                                    unnameable));
    }
    if (capped) {
        ctx.report.Warn(Report::ReasonCode::kPartialByDesign,
                        std::format("stopped after {} containers; the rest were not recorded",
                                    kMaxContainers));
    }
}

void StoredContainers::Apply(Core::ApplyContext& ctx) {
    const auto& payload = ctx.Payload(kId);
    const auto subject = Report::WorldSubject("Containers");

    const auto containers = payload.find("containers");
    if (containers == payload.end() || !containers->is_array()) {
        ctx.report.SkipCategory(Report::ReasonCode::kNone, "no stored containers in the snapshot");
        return;
    }

    if (m_cursor == 0) {
        // Said once, at the start, rather than per container. This category is
        // off by default and this is the reason: it is the one import that
        // reaches out and rewrites the world's own furniture.
        ctx.report.Info(
            "Stored containers writes into every container in this save that it recognises from "
            "the snapshot, topping each one up to what the other save had in it. That is a lot of "
            "chests, barrels and cupboards - anything the old character ever put something into. "
            "It only ever adds: a container is never emptied, and what this save's own copy "
            "already held stays.");
    }

    auto& resolver = Model::FormResolver::Get();
    const auto perFrame = static_cast<size_t>(Config::MigrationConfig::ItemsPerFrame());
    const size_t end = std::min(containers->size(), m_cursor + std::max<size_t>(perFrame, 1));

    for (; m_cursor < end; ++m_cursor) {
        const auto& entry = (*containers)[m_cursor];
        const auto key = entry.value("form", std::string{});
        const auto name = entry.value("name", std::string{});
        if (key.empty()) {
            continue;
        }

        Report::ReasonCode reason = Report::ReasonCode::kNone;
        auto* ref = resolver.ResolveChecked<RE::TESObjectREFR>(key, reason);
        if (!ref) {
            ++m_containersMissing;
            ctx.report.Failed(subject, std::format("container/{}", key), reason,
                              std::format("the container '{}' is not in this load order", name),
                              key, name);
            continue;
        }

        auto* base = ref->GetBaseObject();
        auto* container = base ? base->As<RE::TESObjectCONT>() : nullptr;
        // Re-derived here rather than trusted from the snapshot, so a second run
        // over the same save is a no-op instead of a doubling. What is added is
        // the shortfall against what is already surplus to the base record.
        const auto alreadyAdded = AddedContents(ref, container);

        const auto items = entry.find("items");
        if (items == entry.end() || !items->is_array()) {
            continue;
        }
        uint32_t addedHere = 0;
        for (const auto& item : *items) {
            const auto itemKey = item.value("form", std::string{});
            const auto itemName = item.value("name", std::string{});
            const int32_t wanted = item.value("count", 0);
            if (itemKey.empty() || wanted <= 0) {
                continue;
            }
            Report::ReasonCode itemReason = Report::ReasonCode::kNone;
            auto* object = resolver.ResolveChecked<RE::TESBoundObject>(itemKey, itemReason);
            if (!object) {
                ctx.report.Failed(subject, std::format("container/{}/{}", key, itemKey), itemReason,
                                  std::format("'{}' could not be resolved for container '{}'",
                                              itemName, name),
                                  itemKey, itemName);
                continue;
            }
            int32_t present = 0;
            if (const auto found = alreadyAdded.find(object); found != alreadyAdded.end()) {
                present = found->second;
            }
            const int32_t shortfall = wanted - present;
            if (shortfall <= 0) {
                continue;
            }
            ref->AddObjectToContainer(object, nullptr, shortfall, nullptr);
            ++addedHere;
            ++m_stacksAdded;
        }

        if (addedHere > 0) {
            ++m_containersFilled;
        }
        ctx.report.Succeeded(subject, std::format("container/{}", key), key,
                             name.empty() ? key : name);
    }

    if (m_cursor < containers->size()) {
        ctx.RequestContinuation();
        return;
    }

    ctx.report.Info(std::format(
        "{} container(s) refilled with {} stack(s); {} recorded container(s) do not exist here",
        m_containersFilled, m_stacksAdded, m_containersMissing));
}

}  // namespace SaveMigration::Categories
