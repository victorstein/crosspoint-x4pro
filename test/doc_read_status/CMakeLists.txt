add_executable(DocReadStatusTest
  DocReadStatusTest.cpp
)

target_include_directories(DocReadStatusTest PRIVATE
  ${REPO_ROOT}/lib
)

target_link_libraries(DocReadStatusTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DocReadStatusTest)
