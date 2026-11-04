cmake_minimum_required(VERSION 3.16)

# The version lives here and nowhere else. The release workflow reads it from this line,
# so there is exactly one place to change it (D15).
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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(lootlocker_core PRIVATE lootlocker_compile_options)
target_compile_definitions(lootlocker_core PRIVATE LOOTLOCKER_BUILDING_LIBRARY)

add_library(lootlocker_auth STATIC
    src/auth/auth.cpp
)
add_library(LootLocker::auth ALIAS lootlocker_auth)
target_link_libraries(lootlocker_auth PUBLIC LootLocker::core PRIVATE lootlocker_compile_options)

add_library(lootlocker_broadcasts STATIC
    src/broadcasts/broadcasts.cpp
)
add_library(LootLocker::broadcasts ALIAS lootlocker_broadcasts)
target_link_libraries(lootlocker_broadcasts PUBLIC LootLocker::core PRIVATE lootlocker_compile_options)

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
