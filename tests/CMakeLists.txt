# Tests are built with the same no-exceptions, no-RTTI contract as the library, because a
# test that only passes with exceptions enabled is not testing what ships (D7).
#
# One executable per area, so a failure names the area and the suites stay independent.

add_executable(test_version test_version.cpp)
target_link_libraries(test_version PRIVATE LootLocker::core lootlocker_compile_options)
# The expected version comes from CMake, not from the generated header, so a stale header
# fails the test instead of agreeing with itself.
target_compile_definitions(test_version PRIVATE
    LOOTLOCKER_EXPECTED_MAJOR=${PROJECT_VERSION_MAJOR}
    LOOTLOCKER_EXPECTED_MINOR=${PROJECT_VERSION_MINOR}
    LOOTLOCKER_EXPECTED_PATCH=${PROJECT_VERSION_PATCH})
add_test(NAME version COMMAND test_version)

add_executable(test_result test_result.cpp)
target_link_libraries(test_result PRIVATE LootLocker::core lootlocker_compile_options)
add_test(NAME result COMMAND test_result)

add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE LootLocker::core lootlocker_compile_options)
add_test(NAME config COMMAND test_config)

# Tier 2 (D14): a tiny consumer that links only one module, proving the target graph and
# the headers work from outside the library. It deliberately does NOT link broadcasts.
#
# Registered as a test as well as built, so the smoke check actually runs the binary. A
# compile-only check would pass even if the program crashed on startup.
add_executable(lootlocker_consumer_auth consumer_auth.cpp)
target_link_libraries(lootlocker_consumer_auth PRIVATE LootLocker::auth lootlocker_compile_options)
add_test(NAME consumer_auth COMMAND lootlocker_consumer_auth)
