#pragma once

#include <string>

namespace SaveMigration::Core {

/// What the player answered to the SkyrimNet questions asked just before a restore.
///
/// Written on the game thread while the prompt chain runs, read on the worker during
/// phase R1 - hence the lock inside. Deliberately *not* persisted anywhere: the
/// questions are asked immediately before the run they govern, so a stale answer can
/// never be applied to a later restore. `Clear()` on every arm keeps that true even if
/// a run is abandoned half-way.
class SkyrimNetImportChoices {
public:
    struct Choices {
        /// The chain actually ran. False means nobody was asked - either SkyrimNet is
        /// absent or the snapshot holds no database - and the category decides for
        /// itself, which preserves the behaviour from before the questions existed.
        bool asked = false;

        /// Q1. False means the whole SkyrimNet step is skipped.
        bool importData = false;

        /// Q2. Copy `prompts/_saves/<old>` to `prompts/_saves/<new>` - the dynamic bio
        /// updates and save-specific character bios.
        bool copyPromptArchive = false;

        /// Q3. Rewrite the old character's name in the narrative text. Both names are
        /// carried so the worker never has to touch the game to find them.
        bool renamePlayer = false;
        std::string oldPlayerName;
        std::string newPlayerName;
    };

    static void Set(const Choices& choices);
    static Choices Get();
    static void Clear();
};

}  // namespace SaveMigration::Core
