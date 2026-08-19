add_executable(VisibleRangeTest
  VisibleRangeTest.cpp
)

target_include_directories(VisibleRangeTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(VisibleRangeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VisibleRangeTest)
