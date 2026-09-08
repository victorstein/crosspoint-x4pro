add_executable(HighlightRowTextTest
  HighlightRowTextTest.cpp
)

target_include_directories(HighlightRowTextTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HighlightRowTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HighlightRowTextTest)
