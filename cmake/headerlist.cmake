# Save-Migration has ~120 translation units spread over categories/, core/, store/, ...
# A CONFIGURE_DEPENDS glob keeps the list honest without a 120-line manual roster;
# CMake re-globs on every build so a newly added category is picked up automatically.
file(GLOB_RECURSE headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
)
