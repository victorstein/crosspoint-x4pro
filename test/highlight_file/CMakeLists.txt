# Only the pure decision logic (util/HighlightFileAction.h) is host-buildable
# -- HighlightFile.cpp itself pulls Arduino.h transitively through
# PersistableStore.h and cannot be built here. See the comment atop
# HighlightFileActionTest.cpp.
add_executable(HighlightFileActionTest
  HighlightFileActionTest.cpp
)

target_include_directories(HighlightFileActionTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(HighlightFileActionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HighlightFileActionTest)
