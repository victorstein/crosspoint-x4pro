#include "MeetingDownloadActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <Memory.h>
#include <WiFi.h>
#include <strings.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"
#include "network/PubMediaJson.h"
#include "util/BookCacheUtils.h"
#include "util/TaskWatchdog.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_CANCEL = 1;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

// Language the EPUBs are fetched in, used both as the API's langwritten value
// and as the key to look for under "files" in its response. The week -> issue
// mapping is language-independent, so the meetings page stays English.
constexpr const char* DOWNLOAD_LANGUAGE = "S";

constexpr size_t HASH_CHUNK_BYTES = 2048;
// Hashing several MB off the SD card runs well past the task watchdog window.
constexpr int HASH_CHUNKS_PER_WATCHDOG_RESET = 64;

constexpr MeetingPub PUBLICATION_ORDER[] = {MeetingPub::Watchtower, MeetingPub::Workbook};

// Mirrors the OPDS download target: the configured folder, created on demand,
// falling back to the SD root so a download is never lost to a failed mkdir.
std::string resolveDownloadFolder() {
  const char* folder = SETTINGS.opdsDownloadFolder;
  if (folder[0] == '\0') return {};
  if (!Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR("MEET", "mkdir failed for %s, using SD root", folder);
    return {};
  }
  return folder;
}
}  // namespace

MeetingDownloadActivity::MeetingDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("MeetingDownload", renderer, mappedInput), UiAppHost(renderer) {}

void MeetingDownloadActivity::onEnter() {
  Activity::onEnter();

  state = State::RESOLVING;
  statusMessage = tr(STR_RESOLVING_WEEK);
  errorMessage.clear();
  currentFilename.clear();
  phaseIndex = 0;
  phaseCount = 0;
  workbookUnavailable = false;
  downloadProgress = 0;
  downloadTotal = 0;
  cancelDownload = false;
  goHomeAfterCancel = false;
  sequencePending = false;

  resetUi();
  app.on(ACTION_CANCEL, &MeetingDownloadActivity::onCancelEvent, this);
  app.setScreen(&MeetingDownloadActivity::rootScreen, this);

  if (WiFi.status() == WL_CONNECTED) {
    sequencePending = true;
    requestUpdate();
    return;
  }

  state = State::WIFI_SELECTION;
  statusMessage = tr(STR_CONNECTING);
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void MeetingDownloadActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void MeetingDownloadActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }
  state = State::RESOLVING;
  statusMessage = tr(STR_RESOLVING_WEEK);
  sequencePending = true;
  requestUpdate();
}

void MeetingDownloadActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<MeetingDownloadActivity*>(user);
  if (self->state != State::DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

void MeetingDownloadActivity::loop() {
  if (state == State::WIFI_SELECTION) return;

  if (sequencePending) {
    sequencePending = false;
    runSequence();
    return;
  }

  if (state == State::DOWNLOADING) return;

  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(tapX, tapY)) {
    finish();
  }
}

void MeetingDownloadActivity::fail(const char* message) {
  state = State::FAILED;
  errorMessage = message;
  requestUpdate();
}

void MeetingDownloadActivity::runSequence() {
  // The first paint has to land before the resolve phases block the loop task
  // for up to a minute each; fetchUrl takes neither a progress nor a cancel hook.
  requestUpdateAndWait();

  HalClock::Date today{};
  IsoWeek week;
  if (!halClock.getDate(today) || !isoWeekFromUtcDate(today.year, today.month, today.day, week)) {
    LOG_ERR("MEET", "RTC has no usable date");
    fail(tr(STR_CLOCK_NOT_SET));
    return;
  }
  LOG_INF("MEET", "Device date %04u-%02u-%02u -> ISO week %u/%02u", static_cast<unsigned>(today.year),
          static_cast<unsigned>(today.month), static_cast<unsigned>(today.day), static_cast<unsigned>(week.year),
          static_cast<unsigned>(week.week));

  downloadFolder = resolveDownloadFolder();

  WolWeekScanner scanner;
  if (!scanWeek(week, scanner)) return;

  phaseCount = scanner.count();
  workbookUnavailable = !scanner.has(MeetingPub::Workbook);

  for (const MeetingPub pub : PUBLICATION_ORDER) {
    if (!scanner.has(pub)) continue;
    ++phaseIndex;
    if (!downloadPublication(pub, scanner.issue(pub))) return;
  }

  state = State::FINISHED;
  requestUpdate();
}

bool MeetingDownloadActivity::scanWeek(const IsoWeek& week, WolWeekScanner& scanner) {
  const std::string url = meetingsPageUrl(week);
  size_t bytes = 0;
  const bool fetched = HttpDownloader::fetchUrl(url, [&scanner, &bytes](const uint8_t* data, const size_t len) {
    bytes += len;
    scanner.feed(reinterpret_cast<const char*>(data), len);
    // Returning false is reported as FILE_ERROR, which is indistinguishable from
    // a transport failure, so the whole body is consumed even once both links
    // have been recovered.
    return true;
  });

  // The page is HTML, not an API: log the byte count so a markup change is
  // diagnosable from the serial log rather than just "not found".
  LOG_INF("MEET", "Week page %s: %zu bytes, %d publication(s)", url.c_str(), bytes, scanner.count());

  if (!fetched) {
    fail(tr(STR_DOWNLOAD_FAILED));
    return false;
  }
  if (scanner.count() == 0) {
    fail(tr(STR_NO_PUBLICATIONS_FOUND));
    return false;
  }
  return true;
}

bool MeetingDownloadActivity::downloadPublication(const MeetingPub pub, const char* issue) {
  state = State::RESOLVING;
  statusMessage = tr(STR_RESOLVING_WEEK);
  currentFilename.clear();
  requestUpdateAndWait();

  // ~900 bytes between the tokenizer's buffer and the extracted fields: too much
  // to put on the loop task's stack.
  auto media = makeUniqueNoThrow<PubMediaJsonParser>(DOWNLOAD_LANGUAGE);
  if (!media) {
    LOG_ERR("MEET", "OOM: pub-media parser");
    fail(tr(STR_DOWNLOAD_FAILED));
    return false;
  }

  const std::string mediaUrl = pubMediaUrl(pub, issue, DOWNLOAD_LANGUAGE);
  const bool fetched = HttpDownloader::fetchUrl(mediaUrl, [&media](const uint8_t* data, const size_t len) {
    media->feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!fetched || !media->found()) {
    LOG_ERR("MEET", "No EPUB link for issue %s (%s)", issue, DOWNLOAD_LANGUAGE);
    fail(tr(STR_DOWNLOAD_FAILED));
    return false;
  }

  const std::string filename = filenameFromUrl(media->url());
  if (filename.empty()) {
    LOG_ERR("MEET", "Unusable media url: %s", media->url());
    fail(tr(STR_DOWNLOAD_FAILED));
    return false;
  }

  std::string destPath;
  destPath.reserve(downloadFolder.size() + filename.size() + 1);
  destPath += downloadFolder;
  destPath += '/';
  destPath += filename;

  currentFilename = filename;
  state = State::DOWNLOADING;
  statusMessage = tr(STR_DOWNLOADING);
  downloadProgress = 0;
  downloadTotal = 0;
  requestUpdateAndWait();

  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      media->url(), destPath,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        // The loop task is blocked for the whole transfer; pump input here so
        // Cancel, Back and the home gesture still work.
        mappedInput.update();
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelDownload = true;
        if (mappedInput.wasHomeGesture()) {
          cancelDownload = true;
          goHomeAfterCancel = true;
        }
        routeTouch(mappedInput);
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancelDownload);

  if (result == HttpDownloader::ABORTED) {
    LOG_INF("MEET", "Download cancelled");
    if (goHomeAfterCancel) {
      onGoHome();
    } else {
      finish();
    }
    return false;
  }
  if (result != HttpDownloader::OK) {
    LOG_ERR("MEET", "Download failed: %d", static_cast<int>(result));
    fail(tr(STR_DOWNLOAD_FAILED));
    return false;
  }

  // These transfers run over unverified TLS (the wolfSSL transport has no CA
  // bundle wired up, so setInsecure() is unconditional), which makes the MD5 the
  // API publishes the only integrity check available.
  if (!matchesChecksum(destPath, media->checksum())) {
    LOG_ERR("MEET", "Checksum mismatch for %s", destPath.c_str());
    Storage.remove(destPath.c_str());
    fail(tr(STR_CHECKSUM_MISMATCH));
    return false;
  }

  // The reading cache is keyed on the path hash and book.bin records no size or
  // mtime, so a revised issue downloaded over an existing copy would otherwise
  // be rendered from the previous issue's sections.
  clearBookCache(destPath);
  LOG_INF("MEET", "Saved %s (%llu bytes advertised)", destPath.c_str(),
          static_cast<unsigned long long>(media->filesize()));
  return true;
}

bool MeetingDownloadActivity::matchesChecksum(const std::string& path, const char* expectedMd5) const {
  if (!expectedMd5 || expectedMd5[0] == '\0') {
    LOG_INF("MEET", "No checksum published for %s", path.c_str());
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("MEET", path, file)) return false;

  auto buffer = makeUniqueNoThrow<uint8_t[]>(HASH_CHUNK_BYTES);
  if (!buffer) {
    LOG_ERR("MEET", "OOM: %u byte hash buffer", static_cast<unsigned>(HASH_CHUNK_BYTES));
    return false;
  }

  MD5Builder md5;
  md5.begin();
  int chunks = 0;
  while (true) {
    const int read = file.read(buffer.get(), HASH_CHUNK_BYTES);
    if (read < 0) {
      LOG_ERR("MEET", "Read error while hashing %s", path.c_str());
      return false;
    }
    if (read == 0) break;
    md5.add(buffer.get(), static_cast<size_t>(read));
    if (++chunks % HASH_CHUNKS_PER_WATCHDOG_RESET == 0) resetTaskWatchdogIfSubscribed();
  }
  md5.calculate();

  char actual[33];
  md5.getChars(actual);
  return strcasecmp(actual, expectedMd5) == 0;
}

void MeetingDownloadActivity::rootScreen(UiScreen& screen, void* user) {
  auto* self = static_cast<MeetingDownloadActivity*>(user);
  self->screenHeader(screen);
  if (self->state == State::FINISHED || self->state == State::FAILED) {
    self->buildResultScreen(screen);
  } else {
    self->buildProgressScreen(screen);
  }
}

void MeetingDownloadActivity::screenHeader(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.takeBottom(static_cast<int16_t>(metrics.buttonHintsHeight));
  screen.spacer(static_cast<int16_t>(metrics.topPadding));
  fui::HeaderProps header;
  header.title = tr(STR_MEETING_PUBLICATIONS);
  header.borderEdges = fui::EdgeBottom;
  screen.header(header);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}

void MeetingDownloadActivity::buildProgressScreen(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lineHeight = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barHeight = 16;
  const int16_t buttonHeight = theme.rowHeight;
  const bool downloading = state == State::DOWNLOADING;

  if (!downloading) {
    screen.centeredText(statusMessage.c_str(), centered);
    return;
  }

  char phase[32] = "";
  if (phaseCount > 0) snprintf(phase, sizeof(phase), "%d / %d", phaseIndex, phaseCount);

  const int16_t blockHeight = static_cast<int16_t>(lineHeight * 3 + barHeight + buttonHeight + gap * 4);
  const fui::Rect body = screen.body();
  if (body.height > blockHeight) screen.spacer(static_cast<int16_t>((body.height - blockHeight) / 2));

  screen.target().text(screen.takeTop(lineHeight, gap), statusMessage.c_str(), centered);
  screen.target().text(screen.takeTop(lineHeight, gap), currentFilename.c_str(), centered);

  const fui::Rect bar = screen.takeTop(barHeight, gap).inset(fui::Insets{0, 50, 0, 50});
  if (downloadTotal > 0) {
    fui::ProgressBarProps progress;
    progress.value = static_cast<int32_t>(downloadProgress);
    progress.max = static_cast<int32_t>(downloadTotal);
    progress.border = fui::Paint::solid(fui::Color::Black);
    progress.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, progress);
  }
  screen.target().text(screen.takeTop(lineHeight, gap), phase, centered);

  const fui::Rect buttonArea = screen.takeTop(buttonHeight);
  const auto buttonWidth = static_cast<int16_t>(buttonArea.width / 3);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{static_cast<int16_t>(buttonArea.x + (buttonArea.width - buttonWidth) / 2),
                                  buttonArea.y, buttonWidth, buttonHeight});
}

void MeetingDownloadActivity::buildResultScreen(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lineHeight = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;

  const bool failed = state == State::FAILED;
  // A missing workbook is an informational note beside the completed downloads,
  // not a failure.
  const bool withNote = !failed && workbookUnavailable;
  const int16_t blockHeight = static_cast<int16_t>(lineHeight * (withNote ? 2 : 1) + (withNote ? gap : 0));
  const fui::Rect body = screen.body();
  if (body.height > blockHeight) screen.spacer(static_cast<int16_t>((body.height - blockHeight) / 2));

  screen.target().text(screen.takeTop(lineHeight, gap), failed ? errorMessage.c_str() : tr(STR_DONE), centered);
  if (withNote) screen.target().text(screen.takeTop(lineHeight), tr(STR_WORKBOOK_UNAVAILABLE), centered);
}

void MeetingDownloadActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const MappedInputManager::Labels labels = state == State::DOWNLOADING
                                                ? mappedInput.mapLabels(tr(STR_CANCEL), "", "", "")
                                                : mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderUi();
  renderer.displayBuffer();
}
