# The join lives beside the nav scanner, so expat comes along with it; the same
# repo sources and flags the firmware builds with.
add_executable(BibleBookJoinTest
  BibleBookJoinTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BibleNavScanner.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(BibleBookJoinTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/expat
)

target_compile_definitions(BibleBookJoinTest PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024)

target_link_libraries(BibleBookJoinTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BibleBookJoinTest)
