#pragma once

#include <string>
#include <string_view>

namespace SaveMigration::Util {

/// On-screen messages, gated on the restore still being the thing the player is
/// watching.
///
/// The plugin keeps working long after the import pass returns: the deferred
/// queue replays items as NPCs load and cells attach, and a couple of file-level
/// jobs finish on the worker. Those are *background repairs during ordinary
/// play*, and a corner message about one of them is noise at best - the player
/// is mid-conversation or mid-fight, has no idea what it refers to, and can do
/// nothing about it. The report file and the log are where that belongs.
///
/// So the rule is: an on-screen notice is only honest while the restore is
/// still running, because that is the only time the player is expecting one.
/// Everything after that is logged and nothing else.
namespace Notice {

/// Show `text` if the restore is still in flight; otherwise log it at info and
/// show nothing. Safe to call from any thread - it hops to the game thread
/// itself.
void DuringRestore(std::string text);

/// Same gate, but says explicitly in the log which subsystem stayed quiet.
void DuringRestore(std::string_view source, std::string text);

}  // namespace Notice

}  // namespace SaveMigration::Util
