add_executable(PubMediaJsonTest
  PubMediaJsonTest.cpp
  ${REPO_ROOT}/src/network/PubMediaJson.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(PubMediaJsonTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/JsonParser
)

target_compile_definitions(PubMediaJsonTest PRIVATE
  PUB_MEDIA_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(PubMediaJsonTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PubMediaJsonTest)
