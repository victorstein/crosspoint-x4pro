#pragma once

#include <cstdint>

// Why a document read did not yield a usable document. Callers that own user
// data MUST distinguish Missing (safe to treat as empty and overwrite) from
// Unreadable / ParseError (never overwrite — the data may still be there).
enum class DocReadStatus : uint8_t {
  Ok,
  Missing,
  Unreadable,
  ParseError,
};

// Classifies a read attempt from its three observable outcomes. Deliberately
// free of Arduino and storage dependencies so it can be unit tested on the host.
constexpr DocReadStatus classifyDocRead(const bool exists, const bool contentEmpty, const bool parseFailed) {
  if (!exists) return DocReadStatus::Missing;
  if (contentEmpty) return DocReadStatus::Unreadable;
  if (parseFailed) return DocReadStatus::ParseError;
  return DocReadStatus::Ok;
}
