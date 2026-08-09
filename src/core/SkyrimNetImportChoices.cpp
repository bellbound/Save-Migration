#include "core/SkyrimNetImportChoices.h"

#include <mutex>

namespace SaveMigration::Core {

namespace {
std::mutex g_mutex;
SkyrimNetImportChoices::Choices g_choices;
}  // namespace

void SkyrimNetImportChoices::Set(const Choices& choices) {
    std::lock_guard lock(g_mutex);
    g_choices = choices;
}

SkyrimNetImportChoices::Choices SkyrimNetImportChoices::Get() {
    std::lock_guard lock(g_mutex);
    return g_choices;
}

void SkyrimNetImportChoices::Clear() {
    std::lock_guard lock(g_mutex);
    g_choices = Choices{};
}

}  // namespace SaveMigration::Core
