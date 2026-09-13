add_executable(ReturnStackTest
  ReturnStackTest.cpp
)

target_include_directories(ReturnStackTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReturnStackTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReturnStackTest)
