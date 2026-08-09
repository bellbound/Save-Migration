#include "model/PerkTreeIndex.h"

#include <unordered_map>
#include <vector>

namespace SaveMigration::Model {

namespace {

/// Vanilla has eighteen skill trees carrying a little over 250 perks between
/// them. Anything below this and the walk has clearly not found the trees at
/// all, rather than found a load order with unusually few - a perk overhaul
/// *replaces* trees, it does not empty them.
constexpr size_t kImplausiblySmall = 40;

/// A depth bound rather than a correctness bound. Perk trees are shallow - the
/// deepest vanilla chain is single digits - so this only exists so a corrupt
/// `children` pointer cannot turn the walk into a hang.
constexpr size_t kMaxNodes = 8192;

}  // namespace

PerkTreeIndex::PerkTreeIndex() {
    auto* handler = RE::TESDataHandler::GetSingleton();
    if (!handler) {
        spdlog::error("PerkTreeIndex: no TESDataHandler; every perk will be treated as unknown");
        return;
    }

    size_t skillsWithTrees = 0;
    size_t nodesVisited = 0;

    for (auto* avif : handler->GetFormArray<RE::ActorValueInfo>()) {
        if (!avif || !avif->perkTree) {
            continue;
        }
        ++skillsWithTrees;

        const char* skillName = avif->GetFullName();
        const std::string skill =
            (skillName && *skillName) ? skillName
                                      : (avif->enumName ? avif->enumName : std::string{});

        // Iterative rather than recursive: the graph is a DAG in practice but
        // nothing in the data guarantees it, and `seen` is needed either way.
        std::vector<RE::BGSSkillPerkTreeNode*> stack{avif->perkTree};
        std::unordered_set<RE::BGSSkillPerkTreeNode*> seen;

        while (!stack.empty() && nodesVisited < kMaxNodes) {
            auto* node = stack.back();
            stack.pop_back();
            if (!node || !seen.insert(node).second) {
                continue;
            }
            ++nodesVisited;

            // The root node of a tree carries no perk of its own; only its
            // descendants do.
            if (node->perk) {
                // A tree node names rank 1 only. Ranks 2..N hang off `nextPerk`
                // and are just as buyable, so the chain is walked too - and
                // bounded, because a cycle in `nextPerk` would otherwise spin.
                auto* rank = node->perk;
                for (int guard = 0; rank && guard < 32; ++guard) {
                    if (m_perks.insert(rank).second && !skill.empty()) {
                        m_skillByPerk.emplace(rank, skill);
                    }
                    rank = rank->nextPerk;
                }
            }

            for (auto* child : node->children) {
                stack.push_back(child);
            }
        }
    }

    spdlog::info("PerkTreeIndex: {} perk(s) reachable from {} skill tree(s), {} node(s) walked",
                 m_perks.size(), skillsWithTrees, nodesVisited);
    if (!Usable()) {
        spdlog::error(
            "PerkTreeIndex: only {} perk(s) found, which is too few to be the real skill trees. "
            "Falling back to the old BGSPerk::data.playable filter. If this is VR, suspect the "
            "ActorValueInfo::perkTree member offset.",
            m_perks.size());
    }
}

const PerkTreeIndex& PerkTreeIndex::Get() {
    static const PerkTreeIndex instance;
    return instance;
}

bool PerkTreeIndex::IsInATree(RE::BGSPerk* perk) const {
    return perk && m_perks.contains(perk);
}

bool PerkTreeIndex::Usable() const { return m_perks.size() >= kImplausiblySmall; }

std::string PerkTreeIndex::SkillOf(RE::BGSPerk* perk) const {
    if (const auto found = m_skillByPerk.find(perk); found != m_skillByPerk.end()) {
        return found->second;
    }
    return {};
}

}  // namespace SaveMigration::Model
