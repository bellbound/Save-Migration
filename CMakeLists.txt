cmake_minimum_required(VERSION 3.21)

project(SaveMigration VERSION 0.0.1 LANGUAGES CXX)

#
# YOU DO NOT NEED TO EDIT ANYTHING BELOW HERE
#

if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()

if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME} ${CMAKE_BUILD_TYPE}")
endif()

# directxtk must be found BEFORE CommonLibSSE: CommonLibSSE's exported target
# names Microsoft::DirectXTK in its link interface, and CMake resolves imported
# targets eagerly at find_package time.
find_package(directxtk CONFIG REQUIRED)
find_package(CommonLibSSE CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

include(cmake/headerlist.cmake)
include(cmake/sourcelist.cmake)
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${headers} ${sources})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

# CMakePresets.json sets these via COMMONLIBSSE_PLATFORM, but build-skse-mods.ps1
# invokes cmake directly and never applies a preset. Declaring them on the target
# means std::min/std::max keep working however the project is configured.
target_compile_definitions(
    "${PROJECT_NAME}"
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

target_include_directories(
    "${PROJECT_NAME}"
    PRIVATE
        ${SIMPLEINI_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
    "${PROJECT_NAME}"
    PRIVATE
        nlohmann_json::nlohmann_json
        unofficial::sqlite3::sqlite3
)

# Generate PDB in Release builds
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/Zi>")
    target_link_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:Release>:/DEBUG>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
endif()
