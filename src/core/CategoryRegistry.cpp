#include "core/CategoryRegistry.h"

#include <algorithm>

#include "config/MigrationConfig.h"
#include "papyrus/ModProbe.h"
#include "util/StringUtil.h"

namespace SaveMigration::Core {

namespace {
std::string LowerCopy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
}  // namespace

CategoryRegistry& CategoryRegistry::Get() {
    static CategoryRegistry instance;
    return instance;
}

void CategoryRegistry::AddGlobal(std::unique_ptr<IGlobalCategory> category) {
    if (!category) {
        return;
    }
    if (m_frozen) {
        spdlog::error("CategoryRegistry: refusing to register '{}' after Freeze()",
                      category->Describe().id);
        return;
    }
    m_registrationOrder.push_back(Entry{category.get(), nullptr, category->Describe().phase});
    m_globals.push_back(std::move(category));
}

void CategoryRegistry::AddActor(std::unique_ptr<IActorCategory> category) {
    if (!category) {
        return;
    }
    if (m_frozen) {
        spdlog::error("CategoryRegistry: refusing to register '{}' after Freeze()",
                      category->Describe().id);
        return;
    }
    m_registrationOrder.push_back(Entry{nullptr, category.get(), category->Describe().phase});
    m_actors.push_back(std::move(category));
}

void CategoryRegistry::Freeze() {
    if (m_frozen) {
        return;
    }

    m_globalOrder.clear();
    m_actorOrder.clear();
    for (auto& category : m_globals) {
        m_globalOrder.push_back(category.get());
    }
    for (auto& category : m_actors) {
        m_actorOrder.push_back(category.get());
    }

    // Stable, so intra-phase order is exactly the RegisterAll.cpp order.
    const auto byPhase = [](const auto* a, const auto* b) {
        return PhaseValue(a->Describe().phase) < PhaseValue(b->Describe().phase);
    };
    std::stable_sort(m_globalOrder.begin(), m_globalOrder.end(), byPhase);
    std::stable_sort(m_actorOrder.begin(), m_actorOrder.end(), byPhase);

    // The unified list: registration order, stably sorted by phase. This is the
    // one the orchestrators walk, so a global and a per-actor category in the same
    // phase run in the order RegisterAll.cpp put them.
    m_ordered = m_registrationOrder;
    std::stable_sort(m_ordered.begin(), m_ordered.end(), [](const Entry& a, const Entry& b) {
        return PhaseValue(a.phase) < PhaseValue(b.phase);
    });

    m_globalById.clear();
    for (auto* category : m_globalOrder) {
        const std::string id(category->Describe().id);
        if (!m_globalById.emplace(id, category).second) {
            spdlog::error("CategoryRegistry: duplicate global category id '{}'", id);
        }
    }
    m_actorById.clear();
    for (auto* category : m_actorOrder) {
        const std::string id(category->Describe().id);
        if (!m_actorById.emplace(id, category).second) {
            spdlog::error("CategoryRegistry: duplicate actor category id '{}'", id);
        }
    }

    m_disabled.clear();
    for (const auto& id : Config::MigrationConfig::DisabledCategories()) {
        m_disabled.insert(LowerCopy(id));
        spdlog::info("CategoryRegistry: category '{}' disabled by INI", id);
    }

    // `[Imports]` can only be populated here: this is the first moment the full
    // category list exists. Registration is in `m_ordered` order, so the section
    // reads down the INI in the order the import actually applies things.
    std::vector<std::string> importIds;
    importIds.reserve(m_ordered.size());
    for (const auto& entry : m_ordered) {
        importIds.emplace_back(entry.Describe().id);
    }
    Config::MigrationConfig::RegisterImportToggles(importIds);

    m_frozen = true;
    spdlog::info("CategoryRegistry: frozen with {} global + {} per-actor categories",
                 m_globalOrder.size(), m_actorOrder.size());
    for (auto* category : m_globalOrder) {
        const auto& d = category->Describe();
        spdlog::debug("  [{}] global {} ({})", PhaseValue(d.phase), d.id, ToString(d.phase));
    }
    for (auto* category : m_actorOrder) {
        const auto& d = category->Describe();
        spdlog::debug("  [{}] actor  {} ({})", PhaseValue(d.phase), d.id, ToString(d.phase));
    }
}

std::vector<Phase> CategoryRegistry::PhasesInOrder() const {
    std::vector<int> values;
    for (auto* category : m_globalOrder) {
        values.push_back(PhaseValue(category->Describe().phase));
    }
    for (auto* category : m_actorOrder) {
        values.push_back(PhaseValue(category->Describe().phase));
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());

    std::vector<Phase> phases;
    phases.reserve(values.size());
    for (int value : values) {
        phases.push_back(static_cast<Phase>(value));
    }
    return phases;
}

std::vector<CategoryRegistry::Entry> CategoryRegistry::EntriesForPhase(Phase phase) const {
    std::vector<Entry> result;
    for (const auto& entry : m_ordered) {
        if (entry.phase == phase) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<IGlobalCategory*> CategoryRegistry::GlobalsForPhase(Phase phase) const {
    std::vector<IGlobalCategory*> result;
    for (auto* category : m_globalOrder) {
        if (category->Describe().phase == phase) {
            result.push_back(category);
        }
    }
    return result;
}

std::vector<IActorCategory*> CategoryRegistry::ActorsForPhase(Phase phase) const {
    std::vector<IActorCategory*> result;
    for (auto* category : m_actorOrder) {
        if (category->Describe().phase == phase) {
            result.push_back(category);
        }
    }
    return result;
}

IActorCategory* CategoryRegistry::FindActor(std::string_view id) const {
    const auto it = m_actorById.find(std::string(id));
    return it == m_actorById.end() ? nullptr : it->second;
}

IGlobalCategory* CategoryRegistry::FindGlobal(std::string_view id) const {
    const auto it = m_globalById.find(std::string(id));
    return it == m_globalById.end() ? nullptr : it->second;
}

bool CategoryRegistry::IsDisabled(std::string_view id) const {
    return m_disabled.contains(LowerCopy(id));
}

bool CategoryRegistry::ShouldRun(const IGlobalCategory& category) const {
    const auto& id = category.Describe().id;
    return !IsDisabled(id) && category.IsAvailable();
}

bool CategoryRegistry::ShouldRun(const IActorCategory& category) const {
    const auto& id = category.Describe().id;
    return !IsDisabled(id) && category.IsAvailable();
}

}  // namespace SaveMigration::Core
