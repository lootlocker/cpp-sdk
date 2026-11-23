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

add_executable(test_timezone test_timezone.cpp)
target_link_libraries(test_timezone PRIVATE LootLocker::core lootlocker_compile_options)
add_test(NAME timezone COMMAND test_timezone)

add_executable(test_transport test_transport.cpp)
target_link_libraries(test_transport PRIVATE LootLocker::core lootlocker_compile_options)
add_test(NAME transport COMMAND test_transport)

add_executable(test_config_obfuscation test_config_obfuscation.cpp)
target_link_libraries(test_config_obfuscation PRIVATE LootLocker::core lootlocker_compile_options)
add_test(NAME config_obfuscation COMMAND test_config_obfuscation)

add_executable(test_text test_text.cpp)
target_link_libraries(test_text PRIVATE LootLocker::core lootlocker_compile_options)
# core::text is internal, so src/ is not on the adopter's include path. A test that
# exercises it has to reach in deliberately.
target_include_directories(test_text PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME text COMMAND test_text)

add_executable(test_log test_log.cpp)
target_link_libraries(test_log PRIVATE LootLocker::core lootlocker_compile_options)
add_test(NAME log COMMAND test_log)

# The executor's state machine. Driven by a fake clock and the public fake transport, so
# nothing sleeps and every timing assertion is exact.
#
# `src/core` is on the include path because the executor is internal (D18): an adopter
# cannot reach it, and a test that exercises it has to reach in deliberately.
add_executable(test_executor test_executor.cpp)
target_link_libraries(test_executor PRIVATE LootLocker::core lootlocker_compile_options)
target_include_directories(test_executor PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME executor COMMAND test_executor)

# The rate limiter (T11a). Driven by a fake clock, so nothing sleeps.
add_executable(test_rate_limiter test_rate_limiter.cpp)
target_link_libraries(test_rate_limiter PRIVATE LootLocker::core lootlocker_compile_options)
target_include_directories(test_rate_limiter PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME rate_limiter COMMAND test_rate_limiter)

# The end-to-end transport test (T11, owner 2026-11-13): a real round trip through the real
# curl transport against a loopback server, plus the proof that `Config::transport` is
# actually consulted.
#
# The loopback server needs the platform socket library, which is why this target links
# ws2_32 on Windows. The curl half is compiled only when curl was built in; the injection
# half always runs.
add_executable(test_transport_e2e test_transport_e2e.cpp)
target_link_libraries(test_transport_e2e PRIVATE LootLocker::core lootlocker_compile_options)
target_include_directories(test_transport_e2e PRIVATE ${CMAKE_SOURCE_DIR}/src/core)

# The loopback server runs on its own thread, so this target needs the platform's thread
# library. On Windows that is ws2_32; on POSIX it is Threads::Threads, which is not folded
# into libc everywhere (glibc before 2.34 needs -lpthread explicitly). Linking only the
# socket library would fail to link on those toolchains.
find_package(Threads REQUIRED)
target_link_libraries(test_transport_e2e PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(test_transport_e2e PRIVATE ws2_32)
endif()
add_test(NAME transport_e2e COMMAND test_transport_e2e)

# A timeout on every test, so a hang fails the job instead of blocking CI until someone
# notices. This is not hypothetical: the loopback server's `Stop()` hung on Linux because
# closing a listening socket does not unblock `accept()` there, and the job sat in the test
# step for twenty minutes before anyone could see why.
set_tests_properties(transport_e2e executor PROPERTIES TIMEOUT 120)

# Tier 2 (D14): a tiny consumer that links only one module, proving the target graph and
# the headers work from outside the library. It deliberately does NOT link broadcasts.
#
# Registered as a test as well as built, so the smoke check actually runs the binary. A
# compile-only check would pass even if the program crashed on startup.
#
# Gated on the module being declared: with -DLOOTLOCKER_MODULE_ONLY=core there is no
# LootLocker::auth to link, and an unconditional link makes `cmake` fail at configure time
# with a target-not-found error. Module-only configurations must still configure cleanly,
# because that is how the module-boundary CI proves isolation.
if(TARGET LootLocker::auth)
    add_executable(lootlocker_consumer_auth consumer_auth.cpp)
    target_link_libraries(lootlocker_consumer_auth PRIVATE LootLocker::auth lootlocker_compile_options)
    add_test(NAME consumer_auth COMMAND lootlocker_consumer_auth)
endif()
