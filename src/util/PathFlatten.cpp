#include "PathFlatten.h"

#include <algorithm>
#include <string>

namespace pathflatten {

std::string toCacheName(const std::string& bookPath) {
  std::string bookName = std::string(bookPath).erase(0, 1);
  std::replace(bookName.begin(), bookName.end(), '/', '_');
  std::replace(bookName.begin(), bookName.end(), '\\', '_');
  const size_t lastDot = bookName.find_last_of('.');
  if (lastDot != std::string::npos) {
    bookName.erase(lastDot);
  }
  return bookName;
}

}  // namespace pathflatten
