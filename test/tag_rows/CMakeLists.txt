add_executable(TagRowsTest
  TagRowsTest.cpp
)

target_include_directories(TagRowsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(TagRowsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TagRowsTest)
