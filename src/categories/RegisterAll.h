#pragma once

namespace SaveMigration::Categories {

/// Build the category list and freeze the registry. Call once at kDataLoaded,
/// after `ModProbe::Resolve()` so availability can be evaluated.
void RegisterAllCategories();

}  // namespace SaveMigration::Categories
