# Tests are built with the same no-exceptions, no-RTTI contract as the library, because a
# test that only passes with exceptions enabled is not testing what ships (D7).

add_executable(lootlocker_tests
    test_version.cpp
)
target_link_libraries(lootlocker_tests PRIVATE LootLocker::core lootlocker_compile_options)

add_test(NAME lootlocker_tests COMMAND lootlocker_tests)

# Tier 2 (D14): a tiny consumer that links only one module, proving the target graph and
# the headers work from outside the library. It deliberately does NOT link broadcasts.
add_executable(lootlocker_consumer_auth
    consumer_auth.cpp
)
target_link_libraries(lootlocker_consumer_auth PRIVATE LootLocker::auth lootlocker_compile_options)
