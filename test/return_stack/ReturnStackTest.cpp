#include <gtest/gtest.h>

#include "activities/reader/ReturnStack.h"

// Back after a citation must land one step back, every time. The ring wraps at
// three, so a fourth citation makes "index by count" read the wrong slot and
// the destructor's origin stop being slots_[0]. These pin both.

namespace {

SavedPosition at(const int spine, const int page) { return {spine, page}; }

SavedPosition popped(ReturnStack& stack) {
  SavedPosition out{-1, -1};
  EXPECT_TRUE(stack.pop(out));
  return out;
}

void expectPosition(const SavedPosition& actual, const int spine, const int page) {
  EXPECT_EQ(actual.spineIndex, spine);
  EXPECT_EQ(actual.pageNumber, page);
}

}  // namespace

TEST(ReturnStack, StartsEmpty) {
  ReturnStack stack;
  EXPECT_EQ(stack.count(), 0);
  EXPECT_EQ(stack.oldest(), nullptr);
}

TEST(ReturnStack, PopOnEmptyFailsAndLeavesTheOutputAlone) {
  ReturnStack stack;
  SavedPosition out{42, 7};
  EXPECT_FALSE(stack.pop(out));
  expectPosition(out, 42, 7);
  EXPECT_EQ(stack.count(), 0);
}

TEST(ReturnStack, PopsInLifoOrder) {
  ReturnStack stack;
  stack.push(at(1, 10));
  stack.push(at(2, 20));
  stack.push(at(3, 30));
  EXPECT_EQ(stack.count(), 3);

  expectPosition(popped(stack), 3, 30);
  expectPosition(popped(stack), 2, 20);
  expectPosition(popped(stack), 1, 10);

  SavedPosition out{};
  EXPECT_FALSE(stack.pop(out)) << "three pushes must yield exactly three pops";
}

TEST(ReturnStack, AFourthPushEvictsTheOldestAndPopsStayOneStepBack) {
  ReturnStack stack;
  stack.push(at(1, 10));
  stack.push(at(2, 20));
  stack.push(at(3, 30));
  stack.push(at(4, 40));
  EXPECT_EQ(stack.count(), ReturnStack::CAPACITY);

  // The newest push lives in slot 0 after the wrap; index-by-count would read slot 2.
  expectPosition(popped(stack), 4, 40);
  expectPosition(popped(stack), 3, 30);
  expectPosition(popped(stack), 2, 20);

  SavedPosition out{};
  EXPECT_FALSE(stack.pop(out)) << "the first push was evicted, not retained";
}

TEST(ReturnStack, OldestIsThePhysicallyOldestEntryNotSlotZero) {
  ReturnStack stack;
  stack.push(at(1, 10));
  expectPosition(*stack.oldest(), 1, 10);

  stack.push(at(2, 20));
  expectPosition(*stack.oldest(), 1, 10);

  stack.push(at(3, 30));
  stack.push(at(4, 40));
  // Slot 0 now holds the 4th push, so the destructor's origin is the 2nd.
  expectPosition(*stack.oldest(), 2, 20);
}

TEST(ReturnStack, OldestFollowsThePopsBackDown) {
  ReturnStack stack;
  stack.push(at(1, 10));
  stack.push(at(2, 20));
  stack.push(at(3, 30));

  popped(stack);
  expectPosition(*stack.oldest(), 1, 10);
  popped(stack);
  expectPosition(*stack.oldest(), 1, 10);
  popped(stack);
  EXPECT_EQ(stack.oldest(), nullptr);
}

TEST(ReturnStack, ClearEmptiesAPartialRing) {
  ReturnStack stack;
  stack.push(at(1, 10));
  stack.push(at(2, 20));
  stack.clear();

  EXPECT_EQ(stack.count(), 0);
  EXPECT_EQ(stack.oldest(), nullptr);
  SavedPosition out{};
  EXPECT_FALSE(stack.pop(out));
}

TEST(ReturnStack, ClearEmptiesAWrappedRing) {
  ReturnStack stack;
  for (int i = 1; i <= 5; i++) stack.push(at(i, i * 10));
  stack.clear();

  EXPECT_EQ(stack.count(), 0);
  EXPECT_EQ(stack.oldest(), nullptr);
  SavedPosition out{};
  EXPECT_FALSE(stack.pop(out));
}

TEST(ReturnStack, PushesAfterAClearStartFromScratch) {
  ReturnStack stack;
  for (int i = 1; i <= 4; i++) stack.push(at(i, i * 10));
  stack.clear();

  stack.push(at(9, 90));
  EXPECT_EQ(stack.count(), 1);
  expectPosition(*stack.oldest(), 9, 90);
  expectPosition(popped(stack), 9, 90);
}

TEST(ReturnStack, UnpushUndoesAPush) {
  ReturnStack stack;
  stack.push(at(1, 10));
  stack.push(at(2, 20));
  stack.unpush();

  EXPECT_EQ(stack.count(), 1);
  expectPosition(*stack.oldest(), 1, 10);
  expectPosition(popped(stack), 1, 10);
}

TEST(ReturnStack, UnpushOnEmptyLeavesTheRingUsable) {
  ReturnStack stack;
  stack.unpush();
  EXPECT_EQ(stack.count(), 0);

  stack.push(at(1, 10));
  stack.push(at(2, 20));
  EXPECT_EQ(stack.count(), 2);
  expectPosition(*stack.oldest(), 1, 10);
  expectPosition(popped(stack), 2, 20);
  expectPosition(popped(stack), 1, 10);
}

TEST(ReturnStack, SurvivesRepeatedWrapping) {
  ReturnStack stack;
  for (int i = 1; i <= 100; i++) stack.push(at(i, i * 10));

  EXPECT_EQ(stack.count(), ReturnStack::CAPACITY);
  expectPosition(*stack.oldest(), 98, 980);
  expectPosition(popped(stack), 100, 1000);
  expectPosition(popped(stack), 99, 990);
  expectPosition(popped(stack), 98, 980);
}
