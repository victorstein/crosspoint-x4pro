add_executable(HighlightGeometryTest
  HighlightGeometryTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/HighlightGeometry.cpp
)

target_include_directories(HighlightGeometryTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(HighlightGeometryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HighlightGeometryTest)
