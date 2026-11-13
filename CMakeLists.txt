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
option(LOOTLOCKER_BUILD_EXAMPLES "Build the examples" ON)

# The libcurl transport (D3).
#
# ON by default, but a missing curl is not an error: a console adopter injects its own
# transport and must not be forced to build curl, and a studio that vendors dependencies
# itself may already have one. When curl is absent the SDK still builds and the default
# transport is simply unavailable, which the client reports rather than crashing on.
option(LOOTLOCKER_WITH_CURL "Build the libcurl transport when curl is available" ON)

if(LOOTLOCKER_WITH_CURL)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        message(STATUS "libcurl ${CURL_VERSION_STRING} found: the default transport is available")
    else()
        message(STATUS "libcurl not found: the default transport is unavailable, inject your own")
    endif()
endif()

# Regenerate the timezone table from data/windows-to-iana.tsv at BUILD time.
#
# OFF by default, and it must stay that way. The generator is Python, and requiring Python
# in every adopter's build pipeline — including consoles, where the toolchain is already
# the hard part — would be a worse trade than a checked-in generated file. The table is
# committed precisely so an adopter never needs the generator.
#
# Packaging turns it on: our release workflow controls its environment, so regenerating
# there is free and guarantees the shipped table matches its source. CI also runs the
# generator directly and fails on any diff, which catches a hand-edited table on every PR.
#
# Python is therefore only needed when this option is ON, and the generation happens
# during the build, not during configure. See the target below for why it must be
# command-bearing rather than an OUTPUT rule.
option(LOOTLOCKER_REGENERATE_TIMEZONE
    "Regenerate src/core/timezone_table.inc from data/windows-to-iana.tsv (needs Python)" OFF)

# Declare only the named feature modules, plus the core they depend on.
#
# This exists so the module-boundary CI job can prove isolation. Configuring the whole
# project and building one target does NOT prove it: CMake resolves the target's link
# dependencies, so if auth started linking broadcasts, building auth would build
# broadcasts too and the job would still pass. With this set, the other feature targets
# are never declared, so a cross-module dependency cannot be satisfied.
#
# A list, so `-DLOOTLOCKER_MODULE_ONLY=auth;broadcasts` declares exactly those two. One
# module is just a list of one. Empty (the default) declares every module.
set(LOOTLOCKER_MODULE_ONLY "" CACHE STRING
    "Declare only these feature modules, semicolon-separated (core is always declared). Empty = all modules.")

# Validate the list rather than silently declaring nothing. A typo in a module name used
# to produce a configure that succeeded and a build with no feature targets at all, which
# is the kind of failure that gets diagnosed as "the SDK is broken".
#
# `core` is accepted as an input even though it is not a feature module: naming it means
# "core and no feature modules", which is how the CI proves core builds alone.
set(LOOTLOCKER_FEATURE_MODULES auth broadcasts)
set(LOOTLOCKER_SELECTABLE_MODULES core ${LOOTLOCKER_FEATURE_MODULES})
foreach(requested IN LISTS LOOTLOCKER_MODULE_ONLY)
    if(NOT requested IN_LIST LOOTLOCKER_SELECTABLE_MODULES)
        message(FATAL_ERROR
            "LOOTLOCKER_MODULE_ONLY contains '${requested}', which is not a module. "
            "Known modules: ${LOOTLOCKER_SELECTABLE_MODULES}.")
    endif()
endforeach()

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
        /W4 /permissive- /EHs-c- /GR- /utf-8
        # fopen is not unsafe; the deprecation is a CRT nudge toward fopen_s, which is
        # not portable. We use plain C stdio deliberately.
        /D_CRT_SECURE_NO_WARNINGS
        # nlohmann's internals reference exception machinery even when parsing is
        # non-throwing, which trips C4530 under /EHs-c-. The SDK itself never throws.
        /wd4530)
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
    src/core/result.cpp
    src/core/config.cpp
    src/core/config_file.cpp
    src/core/config_cli.cpp
    src/core/timezone.cpp
    src/core/log.cpp
    src/core/text.cpp
    src/core/config_obfuscation.cpp
    src/core/header_map.cpp
)

# The curl transport is only compiled when curl was found. The header guards on the same
# macro, so an adopter without curl gets a clean build and no dangling declaration.
if(CURL_FOUND)
    target_sources(lootlocker_core PRIVATE src/core/curl_http_transport.cpp)
    target_link_libraries(lootlocker_core PRIVATE CURL::libcurl)
    target_compile_definitions(lootlocker_core PRIVATE LOOTLOCKER_HAVE_CURL)
endif()

add_library(LootLocker::core ALIAS lootlocker_core)

target_include_directories(lootlocker_core
    PUBLIC
        $<BUILD_INTERFACE:${LOOTLOCKER_GENERATED_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        # nlohmann/json is a submodule pinned to a tag (see .gitmodules). Only the
        # single-header include root is on the path, so `#include <nlohmann/json.hpp>`
        # resolves without exposing the rest of the checkout.
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann-json/single_include
)
target_link_libraries(lootlocker_core PRIVATE lootlocker_compile_options)
target_compile_definitions(lootlocker_core PRIVATE LOOTLOCKER_BUILDING_LIBRARY)

# Each feature module is declared only when it is wanted. See LOOTLOCKER_MODULE_ONLY.
foreach(module IN LISTS LOOTLOCKER_FEATURE_MODULES)
    if(LOOTLOCKER_MODULE_ONLY AND NOT module IN_LIST LOOTLOCKER_MODULE_ONLY)
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


# ---------------------------------------------------------------------------
# Generated timezone table (D26)
#
# The table is checked in, so a normal build never runs this. See
# LOOTLOCKER_REGENERATE_TIMEZONE for why it is opt-in.
#
# This is a COMMAND-BEARING target, deliberately. Declaring the checked-in .inc as an
# OUTPUT does not work: the file already exists, so the build system considers the rule
# satisfied and never runs Python. That meant a stale or hand-edited table would survive
# a build that was supposed to regenerate it, which is the exact failure the option exists
# to prevent.
#
# A command-bearing target always runs when the target is built. It re-runs only when the
# TSV or the generator is newer than the output, which is tracked by a stamp in the build
# directory rather than by the checked-in file's timestamp.
# ---------------------------------------------------------------------------

if(LOOTLOCKER_REGENERATE_TIMEZONE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(LOOTLOCKER_TIMEZONE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/data/windows-to-iana.tsv")
    set(LOOTLOCKER_TIMEZONE_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_timezone_table.py")

    # Command-bearing and ALL, so it is always considered out of date and always runs.
    # An OUTPUT-based rule cannot work here: the .inc is checked in, so the build system
    # satisfies the rule from the existing file and never invokes Python. A stale or
    # hand-edited table then survives a build that was meant to regenerate it, which is
    # the exact failure this option exists to prevent.
    add_custom_target(lootlocker_generate_timezone ALL
        COMMAND Python3::Interpreter "${LOOTLOCKER_TIMEZONE_GENERATOR}"
        DEPENDS "${LOOTLOCKER_TIMEZONE_SOURCE}" "${LOOTLOCKER_TIMEZONE_GENERATOR}"
        COMMENT "Regenerating the timezone table from data/windows-to-iana.tsv"
        VERBATIM)
    add_dependencies(lootlocker_core lootlocker_generate_timezone)

    # A configure-time execute_process is the other option, and it is worse: the generator
    # writes into the source tree, so doing it during configure would make a read-only
    # checkout fail to configure at all.
endif()

