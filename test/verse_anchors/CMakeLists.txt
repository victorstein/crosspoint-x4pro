# expat is compiled here from the repo's own sources with the same flags
# platformio.ini passes, so the scan is tokenised by the identical parser the
# firmware uses. xmltok_impl.c and xmltok_ns.c are #included by xmltok.c.
add_executable(VerseAnchorsTest
  VerseAnchorsTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/VerseAnchors.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(VerseAnchorsTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/expat
)

target_compile_definitions(VerseAnchorsTest PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024)

target_link_libraries(VerseAnchorsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VerseAnchorsTest)
