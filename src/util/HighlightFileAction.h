#pragma once

#include <DocReadStatus.h>

#include <cstddef>
#include <cstdint>

// Pure decision logic behind HighlightFile::load/save.
//
// HighlightFile.cpp includes <PersistableStore.h>, which includes <Arduino.h>
// unconditionally -- a genuinely ESP32-specific header (FreeRTOS, esp32-hal,
// pins_arduino, soc/gpio_reg) with no host stub anywhere in this repo, and
// too costly to fake convincingly. So HighlightFile.cpp itself cannot be
// built on the host. This header carries the branch logic that decides
// whether a load recovers, refuses or fails, and whether a save is allowed
// to write at all -- free of Arduino, HalStorage and PersistableStore, the
// same shape as classifyDocRead in Serialization/DocReadStatus.h -- so that
// logic can still be exhaustively host-tested even though the I/O around it
// cannot be.

// What load() should do next, given the primary file's read status and what
// is known about `<path>.tmp`. tempExists/tempParsed only matter when
// primary == Missing: a `.tmp` is never consulted when the primary file is
// present (whether readable or not), because it must never overwrite or
// second-guess a file that may still hold the user's data.
enum class HighlightLoadAction : uint8_t {
  UseLoaded,              // primary parsed -- use it
  ReportEmpty,             // genuinely nothing on disk
  PromoteTempAndUseIt,      // .tmp is the only surviving copy; rescue it now
  DeleteTempReportEmpty,   // .tmp exists but is unusable; discard it
  ReportFailed,             // primary bytes exist but could not be read/parsed
};

constexpr HighlightLoadAction highlightLoadAction(const DocReadStatus primary, const bool tempExists,
                                                   const bool tempParsed) {
  switch (primary) {
    case DocReadStatus::Ok:
      return HighlightLoadAction::UseLoaded;
    case DocReadStatus::Unreadable:
    case DocReadStatus::ParseError:
      return HighlightLoadAction::ReportFailed;
    case DocReadStatus::Missing:
    default:
      if (!tempExists) return HighlightLoadAction::ReportEmpty;
      return tempParsed ? HighlightLoadAction::PromoteTempAndUseIt : HighlightLoadAction::DeleteTempReportEmpty;
  }
}

// Whether save() may write, given the serialised size it measured. Checked
// BEFORE any file is touched: measure first, refuse over budget, write only
// on Write -- so a refusal never creates or modifies a directory or file.
enum class HighlightSaveAction : uint8_t { Write, RefuseTooLarge };

constexpr HighlightSaveAction highlightSaveAction(const size_t measuredBytes, const size_t budget) {
  return measuredBytes > budget ? HighlightSaveAction::RefuseTooLarge : HighlightSaveAction::Write;
}
