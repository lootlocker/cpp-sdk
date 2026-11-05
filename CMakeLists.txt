cmake_minimum_required(VERSION 3.16)

# The version lives here and nowhere else. `configure_file` below turns it into
# include/lootlocker/version.h, so the compile-time constants and GetVersionString() cannot
# drift from this line (D15).
project(LootLocker VERSION 0.1.0 LANGUAGES CXX)

# C++17 is the floor (D1). It is a hard requirement, not a preference: the SDK uses
# std::optional, std::string_view and std::variant.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOOTLOCKER_BUILD_TESTS "Build the SDK's own tests" ON)
option(LOOTLOCKER_BUILD_EXAMPLES "Build the examples" ON)

# Declare only one feature module, plus the core it depends on.
#
# This exists so the module-boundary CI job can prove isolation. Configuring the whole
# project and building one target does NOT prove it: CMake resolves the target's link
# dependencies, so if auth started linking broadcasts, building auth would build
# broadcasts too and the job would still pass. With this set, the other feature targets
# are never declared, so a cross-module dependency cannot be satisfied.
#
# Empty (the default) declares every module.
set(LOOTLOCKER_MODULE_ONLY "" CACHE STRING
    "Declare only this feature module (core is always declared). Empty = all modules.")

# No exceptions and no RTTI in the public contract (D7). These are applied to our targets
# only, never to a consumer's.
add_library(lootlocker_compile_options INTERFACE)
target_compile_features(lootlocker_compile_options INTERFACE cxx_std_17)

if(MSVC)
    # CMake adds /EHsc by default; we replace it with /EHs-c- (no exceptions, no unwind
    # semantics). Clearing the default first avoids the D9025 "overriding" warning on
    # every translation unit.
    string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    target_compile_options(lootlocker_compile_options INTERFACE
        /W4 /permissive- /EHs-c- /GR- /utf-8)
else()
    target_compile_options(lootlocker_compile_options INTERFACE
        -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
endif()

# ---------------------------------------------------------------------------
# Generated version header
#
# The single source of truth is the `project(... VERSION ...)` line above. Both the
# compile-time constants and GetVersionString() come from it, so bumping the version is a
# one-line change and the test below can catch drift.
# ---------------------------------------------------------------------------

set(LOOTLOCKER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
    "${LOOTLOCKER_GENERATED_DIR}/lootlocker/version.h"
    @ONLY
)

# ---------------------------------------------------------------------------
# Targets (D17)
#
# One target per feature module. There is deliberately NO umbrella target: a game names
# the modules it uses, and a module depends on LootLocker::core and on nothing else.
# ---------------------------------------------------------------------------

add_library(lootlocker_core STATIC
    src/core/version.cpp
)
add_library(LootLocker::core ALIAS lootlocker_core)

target_include_directories(lootlocker_core
    PUBLIC
        $<BUILD_INTERFACE:${LOOTLOCKER_GENERATED_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(lootlocker_core PRIVATE lootlocker_compile_options)
target_compile_definitions(lootlocker_core PRIVATE LOOTLOCKER_BUILDING_LIBRARY)

# Each feature module is declared only when it is wanted. See LOOTLOCKER_MODULE_ONLY.
set(LOOTLOCKER_FEATURE_MODULES auth broadcasts)

foreach(module IN LISTS LOOTLOCKER_FEATURE_MODULES)
    if(LOOTLOCKER_MODULE_ONLY AND NOT LOOTLOCKER_MODULE_ONLY STREQUAL module)
        continue()
    endif()

    add_library(lootlocker_${module} STATIC "src/${module}/${module}.cpp")
    add_library(LootLocker::${module} ALIAS lootlocker_${module})
    target_link_libraries(lootlocker_${module}
        PUBLIC LootLocker::core
        PRIVATE lootlocker_compile_options)
endforeach()

# ---------------------------------------------------------------------------
# Tests and examples
# ---------------------------------------------------------------------------

if(LOOTLOCKER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(LOOTLOCKER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()


