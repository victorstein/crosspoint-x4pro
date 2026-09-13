#pragma once

// Positions the reader returns to when Back is pressed after following a
// citation. The ring lives here, free of firmware includes, so the wrap
// arithmetic can be tested on the host: reproducing a wrap on the device means
// following four citations in a row and noticing which of the four Back lands
// on, and an index-by-count read of a wrapped ring is off by one.
struct SavedPosition {
  int spineIndex;
  int pageNumber;
};

class ReturnStack {
 public:
  static constexpr int CAPACITY = 3;

  // At capacity the oldest entry is evicted, trading the article origin for
  // every individual Back being one correct step back.
  void push(const SavedPosition p) {
    slots_[top_] = p;
    top_ = (top_ + 1) % CAPACITY;
    if (count_ < CAPACITY) count_++;
  }

  bool pop(SavedPosition& out) {
    if (count_ == 0) return false;
    top_ = (top_ + CAPACITY - 1) % CAPACITY;
    count_--;
    out = slots_[top_];
    return true;
  }

  // Undoes a push for a caller that only discovers the navigation failed after
  // pushing. Unlike pop it cannot restore an entry the push evicted.
  void unpush() {
    top_ = (top_ + CAPACITY - 1) % CAPACITY;
    if (count_ > 0) count_--;
  }

  void clear() {
    top_ = 0;
    count_ = 0;
  }

  int count() const { return count_; }

  // Physically-oldest retained entry, which is not slots_[0] once the ring has
  // wrapped.
  const SavedPosition* oldest() const { return count_ == 0 ? nullptr : &slots_[(top_ - count_ + CAPACITY) % CAPACITY]; }

 private:
  SavedPosition slots_[CAPACITY] = {};
  int top_ = 0;
  int count_ = 0;
};
