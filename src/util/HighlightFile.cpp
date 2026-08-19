#include "HighlightFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include "HighlightFileAction.h"
#include "PathFlatten.h"

namespace {

std::string highlightsDir() { return "/.crosspoint/highlights/"; }

std::string highlightPath(const std::string& bookPath) {
  return highlightsDir() + pathflatten::toCacheName(bookPath) + ".json";
}

}  // namespace

namespace HighlightFile {

LoadResult load(const std::string& bookPath, HighlightDoc& doc) {
  const std::string path = highlightPath(bookPath);
  const std::string tmpPath = path + ".tmp";

  JsonDocument primaryJson;
  const DocReadStatus primaryStatus = PersistableStoreBase::readDocFromFileChecked(path.c_str(), primaryJson);

  bool tempExists = false;
  bool tempParsed = false;
  JsonDocument tempJson;
  if (primaryStatus == DocReadStatus::Missing) {
    tempExists = Storage.exists(tmpPath.c_str());
    if (tempExists) {
      tempParsed = PersistableStoreBase::readDocFromFileChecked(tmpPath.c_str(), tempJson) == DocReadStatus::Ok;
    }
  }

  switch (highlightLoadAction(primaryStatus, tempExists, tempParsed)) {
    case HighlightLoadAction::UseLoaded:
      if (doc.fromJson(primaryJson.as<JsonVariantConst>())) return LoadResult::Loaded;
      LOG_ERR("HLFILE", "Rejected %s (future format version?)", path.c_str());
      return LoadResult::Failed;

    case HighlightLoadAction::ReportEmpty:
      return LoadResult::Empty;

    case HighlightLoadAction::PromoteTempAndUseIt: {
      // Promote first: the rename is what rescues the only surviving copy of
      // the user's data. Do this before trusting the parsed content, so a
      // validation failure below can never leave the rescue undone -- the
      // primary path is Missing, so nothing here can be overwritten.
      if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
        LOG_ERR("HLFILE", "Failed to promote %s into place", tmpPath.c_str());
      }
      if (doc.fromJson(tempJson.as<JsonVariantConst>())) return LoadResult::RecoveredFromTemp;
      LOG_ERR("HLFILE", "Recovered %s but rejected its contents (future format version?)", path.c_str());
      return LoadResult::Failed;
    }

    case HighlightLoadAction::DeleteTempReportEmpty:
      Storage.remove(tmpPath.c_str());
      return LoadResult::Empty;

    case HighlightLoadAction::ReportFailed:
      return LoadResult::Failed;
  }
  return LoadResult::Failed;
}

SaveResult save(const std::string& bookPath, const HighlightDoc& doc) {
  JsonDocument json;
  doc.toJson(json);

  if (highlightSaveAction(measureJson(json), SAVE_BYTE_BUDGET) == HighlightSaveAction::RefuseTooLarge) {
    LOG_ERR("HLFILE", "Highlights for %s exceed the save budget; not written", bookPath.c_str());
    return SaveResult::TooLarge;
  }

  // writeDocToFileAtomic only ensures /.crosspoint; the highlights
  // subdirectory is ours, same as BookmarkFile.cpp does for bookmarks.
  Storage.mkdir(highlightsDir().c_str());
  const std::string path = highlightPath(bookPath);
  return PersistableStoreBase::writeDocToFileAtomic(path.c_str(), json) ? SaveResult::Ok : SaveResult::WriteFailed;
}

}  // namespace HighlightFile
