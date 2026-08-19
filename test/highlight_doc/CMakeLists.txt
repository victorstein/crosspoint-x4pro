add_executable(HighlightDocTest
  HighlightDocTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/HighlightDoc.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(HighlightDocTest PRIVATE
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(HighlightDocTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(HighlightDocTest)
