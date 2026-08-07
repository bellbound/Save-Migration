#pragma once

// Keep this deliberately small. RE/Skyrim.h alone is enormous, and MSVC's PCH
// virtual-memory budget (C3859 / C1076) is reached quickly once heavyweight
// header-only libraries are added on top of it. nlohmann/json in particular goes
// in the translation units that need it, not here.
#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/spdlog.h>

// Windows defines GetObject as a macro, which collides with
// RE::BSScript::Variable::GetObject. Nothing in this project wants the macro.
#ifdef GetObject
#    undef GetObject
#endif

using namespace std::literals;
