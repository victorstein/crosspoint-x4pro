#include <gtest/gtest.h>

#include "util/PathFlatten.h"

TEST(PathFlatten, FlattensSeparatorsAndDropsTheExtension) {
  EXPECT_EQ(pathflatten::toCacheName("/books/novel.epub"), "books_novel");
  EXPECT_EQ(pathflatten::toCacheName("/a/b/c.txt"), "a_b_c");
}

TEST(PathFlatten, ReplacesBackslashesToo) { EXPECT_EQ(pathflatten::toCacheName("/a\\b/c.epub"), "a_b_c"); }

TEST(PathFlatten, TruncatesAtAnyDotInThePath) {
  // Inherited: find_last_of('.') runs on the FLATTENED name, so a dot in a
  // directory truncates it. Locked in by test — changing it orphans every
  // existing bookmark file.
  EXPECT_EQ(pathflatten::toCacheName("/v1.0/mybook"), "v1");
}

TEST(PathFlatten, DifferentExtensionsCollide) {
  EXPECT_EQ(pathflatten::toCacheName("/books/x.epub"), pathflatten::toCacheName("/books/x.txt"));
}

TEST(PathFlatten, StripsTheFirstCharacterUnconditionally) {
  // erase(0,1) is unconditional, so a path without a leading slash loses a real
  // character. Documented rather than fixed, for the same compatibility reason.
  EXPECT_EQ(pathflatten::toCacheName("books/x.epub"), "ooks_x");
}
