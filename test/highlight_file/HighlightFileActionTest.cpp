// Host coverage for HighlightFile's decision logic.
//
// HighlightFile.cpp itself cannot be built on the host: it includes
// <PersistableStore.h>, which includes <Arduino.h> unconditionally, and
// Arduino.h reaches FreeRTOS, esp32-hal and other ESP32-only headers that
// have no stub in test/stubs and are not worth faking convincingly. See the
// comment atop util/HighlightFileAction.h for the full reasoning.
//
// So this suite exhaustively drives the pure branch logic -- the .tmp
// promotion decision, the never-overwrite-on-failure rule, and the
// before-any-write budget check -- and the actual Storage call sequence in
// HighlightFile.cpp remains device-verified only (pio build for both boards).

#include <gtest/gtest.h>

#include "util/HighlightFileAction.h"

TEST(HighlightLoadAction, PrimaryOkAlwaysUsesTheLoadedDoc) {
  EXPECT_EQ(highlightLoadAction(DocReadStatus::Ok, false, false), HighlightLoadAction::UseLoaded);
  EXPECT_EQ(highlightLoadAction(DocReadStatus::Ok, true, false), HighlightLoadAction::UseLoaded);
  EXPECT_EQ(highlightLoadAction(DocReadStatus::Ok, false, true), HighlightLoadAction::UseLoaded);
  EXPECT_EQ(highlightLoadAction(DocReadStatus::Ok, true, true), HighlightLoadAction::UseLoaded)
      << "a readable primary is never second-guessed by a .tmp, present or not";
}

TEST(HighlightLoadAction, PrimaryUnreadableAlwaysFailsRegardlessOfTemp) {
  EXPECT_EQ(highlightLoadAction(DocReadStatus::Unreadable, false, false), HighlightLoadAction::ReportFailed);
  EXPECT_EQ(highlightLoadAction(DocReadStatus::Unreadable, true, true), HighlightLoadAction::ReportFailed)
      << "an unreadable primary must not fall back to .tmp -- it may still hold the data";
}

TEST(HighlightLoadAction, PrimaryParseErrorAlwaysFailsRegardlessOfTemp) {
  EXPECT_EQ(highlightLoadAction(DocReadStatus::ParseError, false, false), HighlightLoadAction::ReportFailed);
  EXPECT_EQ(highlightLoadAction(DocReadStatus::ParseError, true, true), HighlightLoadAction::ReportFailed)
      << "an unparseable primary must not fall back to .tmp -- it may still hold the data";
}

TEST(HighlightLoadAction, MissingPrimaryWithNoTempIsGenuinelyEmpty) {
  EXPECT_EQ(highlightLoadAction(DocReadStatus::Missing, false, false), HighlightLoadAction::ReportEmpty);
  EXPECT_EQ(highlightLoadAction(DocReadStatus::Missing, false, true), HighlightLoadAction::ReportEmpty)
      << "tempParsed is meaningless when tempExists is false";
}

TEST(HighlightLoadAction, MissingPrimaryWithAParsedTempIsPromoted) {
  EXPECT_EQ(highlightLoadAction(DocReadStatus::Missing, true, true), HighlightLoadAction::PromoteTempAndUseIt);
}

TEST(HighlightLoadAction, MissingPrimaryWithAnUnparseableTempIsDiscarded) {
  EXPECT_EQ(highlightLoadAction(DocReadStatus::Missing, true, false), HighlightLoadAction::DeleteTempReportEmpty);
}

TEST(HighlightSaveAction, WritesWhenAtOrUnderBudget) {
  EXPECT_EQ(highlightSaveAction(0, 45000), HighlightSaveAction::Write);
  EXPECT_EQ(highlightSaveAction(44999, 45000), HighlightSaveAction::Write);
  EXPECT_EQ(highlightSaveAction(45000, 45000), HighlightSaveAction::Write) << "the budget itself must still fit";
}

TEST(HighlightSaveAction, RefusesOneByteOverBudget) {
  EXPECT_EQ(highlightSaveAction(45001, 45000), HighlightSaveAction::RefuseTooLarge);
}

TEST(HighlightSaveAction, RefusesTheDocumentedWorstCase) {
  // HighlightDocTest.WorstCaseDocumentStaysUnderTheSaveBudget records the
  // real worst case as > 50000 bytes; this is the guard that must catch it.
  EXPECT_EQ(highlightSaveAction(90733, 45000), HighlightSaveAction::RefuseTooLarge);
}
