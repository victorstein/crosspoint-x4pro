add_executable(MonthNameMapTest
  MonthNameMapTest.cpp
  ${REPO_ROOT}/src/network/WolWeekScan.cpp
)

target_include_directories(MonthNameMapTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(MonthNameMapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MonthNameMapTest)
