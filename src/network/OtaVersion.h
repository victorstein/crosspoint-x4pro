#pragma once

#include <cstdio>
#include <cstring>

// Header-only and free of Logging.h / CROSSPOINT_VERSION so the host suite
// (test/ota_version) can exercise the comparison directly.
namespace ota_version {

inline bool parseVersionTriple(const char* version, int& major, int& minor, int& patch) {
  if (!version) return false;
  if (*version == 'v' || *version == 'V') ++version;
  return std::sscanf(version, "%d.%d.%d", &major, &minor, &patch) == 3;
}

inline bool isNewerVersion(const char* latest, const char* current) {
  if (!latest || !*latest || !current) return false;

  // A device flashed over the air reports exactly the tag it was built from, so
  // this is the common "already up to date" path. It is also what stops the -rc
  // tie-break below from offering an RC build its own running image forever.
  if (std::strcmp(latest, current) == 0) return false;

  int latestMajor = 0, latestMinor = 0, latestPatch = 0;
  int currentMajor = 0, currentMinor = 0, currentPatch = 0;
  // A tag that does not yield three integers is treated as no update: sscanf
  // leaves its outputs untouched on a partial match, so comparing anyway would
  // compare uninitialised values.
  if (!parseVersionTriple(latest, latestMajor, latestMinor, latestPatch)) return false;
  if (!parseVersionTriple(current, currentMajor, currentMinor, currentPatch)) return false;

  if (latestMajor != currentMajor) return latestMajor > currentMajor;
  if (latestMinor != currentMinor) return latestMinor > currentMinor;
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // An RC carries the triple of the release it precedes, so the plain tag of
  // that same triple supersedes it.
  return std::strstr(current, "-rc") != nullptr;
}

}  // namespace ota_version
