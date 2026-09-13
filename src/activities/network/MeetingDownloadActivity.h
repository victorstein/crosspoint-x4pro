#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "network/WolWeekScan.h"

/**
 * Downloads the current week's meeting publications — the Watchtower study
 * edition and the Life and Ministry Meeting Workbook — as EPUBs onto the SD
 * card.
 *
 * The chain is device clock -> ISO week -> the week's wol.jw.org meetings page
 * -> issue numbers parsed out of the publication link paths -> GETPUBMEDIALINKS
 * -> download. Either publication may be absent for a given week, so the
 * progress phases are sized from what the page scan actually found.
 */
class MeetingDownloadActivity final : public Activity, private UiAppHost {
 public:
  enum class State { WIFI_SELECTION, RESOLVING, DOWNLOADING, FINISHED, FAILED };

  explicit MeetingDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static void rootScreen(UiScreen& screen, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);

  void screenHeader(UiScreen& screen);
  void buildProgressScreen(UiScreen& screen);
  void buildResultScreen(UiScreen& screen);

  void onWifiSelectionComplete(bool connected);
  void runSequence();
  bool scanWeek(const IsoWeek& week, WolWeekScanner& scanner);
  bool downloadPublication(MeetingPub pub, const char* issue);
  bool matchesChecksum(const std::string& path, const char* expectedMd5) const;
  void fail(const char* message);

  // "" when no folder is configured or it could not be created, meaning SD root.
  std::string downloadFolder;

  State state = State::RESOLVING;
  // Set once the screen has been painted, so the blocking resolve/download work
  // starts from loop() rather than from onEnter().
  bool sequencePending = false;
  std::string statusMessage;
  std::string errorMessage;
  // Name of the file currently being written, shown under the progress bar.
  std::string currentFilename;
  // 1-based phase and the number of publications this week actually references.
  int phaseIndex = 0;
  int phaseCount = 0;
  // A week with no workbook is a normal outcome worth a note, not an error.
  bool workbookUnavailable = false;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  // Read by HttpDownloader between chunks; set by the Cancel button or a Back
  // press, both pumped from the download's own progress callback.
  bool cancelDownload = false;
  bool goHomeAfterCancel = false;

  bool preventAutoSleep() override { return true; }
};
