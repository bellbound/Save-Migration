#pragma once

#include <string_view>
#include <vector>

#include "core/Category.h"
#include "store/ModFiles.h"

namespace SaveMigration::Categories {

/// One Mod Support entry: a whole category described by data rather than by code.
///
/// The point of the table is that adding support for a mod whose settings are
/// files should not need a new class, a new payload shape or a new pair of INI
/// keys - only its paths. Everything else, including the export and import
/// switches and the two rows in the menu, falls out of the id.
struct ModBundle {
    std::string_view id;
    std::string_view displayName;
    /// Shown in the menu, on both pages, as the row's info text.
    ///
    /// Written here rather than in the Papyrus so there is one source for it: the
    /// menu had begun to accumulate prose about individual categories, which is
    /// prose about a thing the menu does not own.
    std::string_view description;
    Store::ModFileSpec spec;
};

/// Every bundle, in menu order. The one place to edit to add a mod.
const std::vector<ModBundle>& ModBundles();

/// The bundle with this id, or null.
const ModBundle* FindModBundle(std::string_view categoryId);

/// A category that carries one mod's files, whole.
///
/// Deliberately knows nothing about what any of them mean. That is what lets it
/// support a mod nobody has written code for - the format of a settings file is
/// its mod's business, and staying out of it is the feature. The cost is that
/// nothing here can be validated the way `system.tng_settings` validates every id
/// it writes, so these are files-in, files-out and the report says so.
class ModSupportCategory final : public Core::IGlobalCategory {
public:
    explicit ModSupportCategory(const ModBundle& bundle);

    [[nodiscard]] const Core::CategoryDescriptor& Describe() const override;
    void Collect(Core::CollectContext& ctx) override;
    void Apply(Core::ApplyContext& ctx) override;

private:
    /// A reference into `ModBundles()`, which is a function-local static and
    /// therefore outlives every category.
    const ModBundle& m_bundle;
    /// Per instance, unlike every other category's, because the descriptor *is*
    /// the per-instance data here. Its `string_view` fields point at the table's
    /// literals.
    Core::CategoryDescriptor m_descriptor;
};

}  // namespace SaveMigration::Categories
