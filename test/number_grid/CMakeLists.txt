add_executable(NumberGridLayoutTest
  NumberGridLayoutTest.cpp
)

target_include_directories(NumberGridLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(NumberGridLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(NumberGridLayoutTest)
