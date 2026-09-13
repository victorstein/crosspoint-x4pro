add_executable(MeetingFilenameTest
  MeetingFilenameTest.cpp
  ${REPO_ROOT}/src/network/MeetingFilename.cpp
  ${REPO_ROOT}/src/network/WolWeekScan.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(MeetingFilenameTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(MeetingFilenameTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MeetingFilenameTest)
