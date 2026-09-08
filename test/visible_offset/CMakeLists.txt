add_executable(VisibleOffsetTest
  VisibleOffsetTest.cpp
)

target_include_directories(VisibleOffsetTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub
)

target_link_libraries(VisibleOffsetTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VisibleOffsetTest)
