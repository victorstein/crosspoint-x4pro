add_executable(WolWeekScanTest
  WolWeekScanTest.cpp
  ${REPO_ROOT}/src/network/WolWeekScan.cpp
)

target_include_directories(WolWeekScanTest PRIVATE
  ${REPO_ROOT}/src
)

target_compile_definitions(WolWeekScanTest PRIVATE
  WOL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(WolWeekScanTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WolWeekScanTest)
