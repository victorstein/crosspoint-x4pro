add_executable(BitBlitTest
  BitBlitTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitBlit.cpp
)

target_include_directories(BitBlitTest PRIVATE
  ${REPO_ROOT}/lib
)

target_link_libraries(BitBlitTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BitBlitTest)
