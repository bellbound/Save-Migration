set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)

# MATCHES takes an unanchored regex, not a glob. The upstream template writes this
# as "...|qt*", which as a regex is "q followed by zero or more t" and therefore
# matches any port whose name merely contains a q -- including sqlite3. That made
# sqlite3 build dynamic, so SaveMigration.dll imported sqlite3.dll, which Windows
# never finds next to a plugin (dependencies resolve from the exe dir, not the
# DLL's own dir) and SKSE failed the load with error 126. Anchor it and spell the
# prefix glob as qt.* so only genuine Qt ports match.
if ("${PORT}" MATCHES "^(fully-dynamic-game-engine|skse|qt.*)$")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
else ()
    set(VCPKG_LIBRARY_LINKAGE static)
endif ()
