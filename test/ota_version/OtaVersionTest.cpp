#include <gtest/gtest.h>

#include "network/OtaVersion.h"

// The release tag decides whether the device overwrites its own firmware, so
// every way a tag can be malformed has to fail closed. sscanf leaves its outputs
// untouched on a partial match, and the identical-version short-circuit is load
// bearing for RC builds: without it an RC offers itself its own running image.

namespace {

bool newer(const char* latest, const char* current) { return ota_version::isNewerVersion(latest, current); }

}  // namespace

TEST(OtaVersion, HigherTripleIsNewer) {
  EXPECT_TRUE(newer("1.6.0", "1.5.0"));
  EXPECT_TRUE(newer("1.5.1", "1.5.0"));
  EXPECT_TRUE(newer("2.0.0", "1.9.9"));
}

TEST(OtaVersion, LowerTripleIsNotNewer) {
  EXPECT_FALSE(newer("1.4.0", "1.5.0"));
  EXPECT_FALSE(newer("1.5.0", "1.5.1"));
  EXPECT_FALSE(newer("1.9.9", "2.0.0"));
}

TEST(OtaVersion, IdenticalVersionIsNotNewer) { EXPECT_FALSE(newer("1.5.0", "1.5.0")); }

TEST(OtaVersion, SideloadedDevBuildOfTheSameTripleIsNotNewer) { EXPECT_FALSE(newer("1.5.0", "1.5.0-x4pro")); }

TEST(OtaVersion, LeadingVIsIgnoredRatherThanOfferingAReinstall) {
  EXPECT_FALSE(newer("v1.5.0", "1.5.0"));
  EXPECT_FALSE(newer("V1.5.0", "1.5.0"));
}

TEST(OtaVersion, LeadingVTagComparesLikeTheBareTag) {
  EXPECT_TRUE(newer("v1.6.0", "1.5.0"));
  EXPECT_TRUE(newer("V1.6.0", "1.5.0"));
  EXPECT_FALSE(newer("v1.4.0", "1.5.0"));
}

TEST(OtaVersion, PartialTagsDoNotCompare) {
  EXPECT_FALSE(newer("1.5", "1.5.0"));
  EXPECT_FALSE(newer("2.0", "1.9.9"));
  EXPECT_FALSE(newer("2", "1.9.9"));
  EXPECT_FALSE(newer("v1.6", "1.5.0"));
}

TEST(OtaVersion, GarbageTagsAreNotNewer) {
  EXPECT_FALSE(newer("", "1.5.0"));
  EXPECT_FALSE(newer("abc", "1.5.0"));
  EXPECT_FALSE(newer("latest", "1.5.0"));
  EXPECT_FALSE(newer("v", "1.5.0"));
  EXPECT_FALSE(newer(nullptr, "1.5.0"));
}

TEST(OtaVersion, UnparseableCurrentVersionIsNotNewer) {
  EXPECT_FALSE(newer("1.6.0", ""));
  EXPECT_FALSE(newer("1.6.0", "abc"));
  EXPECT_FALSE(newer("1.6.0", "1.5"));
  EXPECT_FALSE(newer("1.6.0", nullptr));
}

TEST(OtaVersion, ReleaseSupersedesItsOwnReleaseCandidate) {
  EXPECT_TRUE(newer("1.5.0", "1.5.0-rc+abc1234"));
  EXPECT_TRUE(newer("v1.5.0", "1.5.0-rc+abc1234"));
}

TEST(OtaVersion, ReleaseCandidateIsNotOfferedItsOwnRunningImage) {
  EXPECT_FALSE(newer("1.5.0-rc+abc1234", "1.5.0-rc+abc1234"));
}

TEST(OtaVersion, AnOlderReleaseDoesNotSupersedeANewerReleaseCandidate) {
  EXPECT_FALSE(newer("1.4.0", "1.5.0-rc+abc1234"));
}
