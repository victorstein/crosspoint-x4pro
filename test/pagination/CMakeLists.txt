# Runs the real ParsedText / TextBlock pagination code on the host.
#
# GfxRenderer has no virtual functions, so the fake is a LINK-TIME substitute:
# the real GfxRenderer.h is compiled unmodified and GfxRendererFake.cpp supplies
# the bodies. A new renderer call from layout code therefore breaks this link
# rather than silently diverging.
#
# lib/hal and lib/Logging are deliberately absent from the include path below.
# <HalDisplay.h>, <HalStorage.h> and <Logging.h> resolve to test/stubs instead,
# which keeps <Arduino.h> and the freeink-sdk panel chain out of the build.
add_executable(PaginationInvarianceTest
  PaginationInvarianceTest.cpp
  GfxRendererFake.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(PaginationInvarianceTest PRIVATE
  ${REPO_ROOT}/test/stubs  # no-op Logging.h / HalDisplay.h / HalStorage.h
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(PaginationInvarianceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PaginationInvarianceTest)
