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

# The target graph lands with the first feature module. Until then this project is a
# placeholder, so that a consumer's `add_subdirectory` and the CI matrix have something
# valid to configure.
#
# The shape it will take (D17): one target per feature module — LootLocker::core,
# LootLocker::auth, LootLocker::broadcasts — and deliberately no umbrella target.
message(STATUS "LootLocker C++ SDK ${PROJECT_VERSION} - scaffolding only, no targets yet")
