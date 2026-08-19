add_executable(Utf8SummaryTest
  Utf8SummaryTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(Utf8SummaryTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(Utf8SummaryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Utf8SummaryTest)
