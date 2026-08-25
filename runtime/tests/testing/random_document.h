// Test-only generator of random Document values for property-based round-trip
// tests. Deterministically seeded by the caller so failures reproduce.

#ifndef SMITHY_TESTS_TESTING_RANDOM_DOCUMENT_H_
#define SMITHY_TESTS_TESTING_RANDOM_DOCUMENT_H_

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "smithy/core/document.h"

namespace smithy::testing {

struct DocumentGeneratorOptions {
  // JSON cannot represent blob/timestamp nodes natively; codecs that re-type
  // them (base64 text / epoch numbers) lose node identity on decode, so the
  // JSON property test disables them.
  bool with_blobs = true;
  bool with_timestamps = true;
  int max_depth = 4;
};

class RandomDocumentGenerator {
 public:
  explicit RandomDocumentGenerator(std::uint64_t seed, DocumentGeneratorOptions options = {})
      : rng_(seed), options_(options) {}

  Document Next() { return Generate(options_.max_depth); }

 private:
  int Range(int inclusive_max) {
    return std::uniform_int_distribution<int>(0, inclusive_max)(rng_);
  }

  std::string RandomText() {
    static constexpr char kAlphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-./\\\"{}[]:,\xC3\xA9";
    std::string out;
    const int length = Range(24);
    out.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
      // Stay within the leading ASCII range plus one valid UTF-8 pair.
      const int index = Range(static_cast<int>(sizeof(kAlphabet)) - 4);
      out.push_back(kAlphabet[index]);
    }
    return out;
  }

  Document Generate(int depth) {  // NOLINT(misc-no-recursion)
    const int scalar_kinds = 5 + (options_.with_blobs ? 1 : 0) + (options_.with_timestamps ? 1 : 0);
    const int kind = depth <= 0 ? Range(scalar_kinds - 1) : Range(scalar_kinds + 1);
    switch (kind) {
      case 0:
        return Document(nullptr);
      case 1:
        return Document(Range(1) == 0);
      case 2:
        return Document(std::uniform_int_distribution<std::int64_t>(
            std::numeric_limits<std::int64_t>::min(),
            std::numeric_limits<std::int64_t>::max())(rng_));
      case 3: {
        // Non-integral double, safely representable in every backend.
        const double value = std::uniform_real_distribution<double>(-1.0e12, 1.0e12)(rng_) + 0.25;
        return Document(value);
      }
      case 4:
        return Document(RandomText());
      case 5: {
        if (options_.with_blobs) {
          std::vector<std::uint8_t> bytes(static_cast<std::size_t>(Range(32)));
          for (auto& byte : bytes) {
            byte = static_cast<std::uint8_t>(Range(255));
          }
          return Document(Blob(std::move(bytes)));
        }
        return NestedOrString(depth);
      }
      case 6: {
        if (options_.with_timestamps) {
          // +/- ~year 2100 in milliseconds: survives the double round trip of
          // CBOR fractional-seconds encoding exactly.
          const std::int64_t ms = std::uniform_int_distribution<std::int64_t>(
              -4'000'000'000'000LL, 4'000'000'000'000LL)(rng_);
          return Document(
              TimestampValue{Timestamp::FromEpochMilliseconds(ms), TimestampFormat::kEpochSeconds});
        }
        return NestedOrString(depth);
      }
      default:
        return NestedOrString(depth);
    }
  }

  Document NestedOrString(int depth) {  // NOLINT(misc-no-recursion)
    if (depth <= 0) {
      return Document(RandomText());
    }
    if (Range(1) == 0) {
      DocumentList list;
      const int size = Range(4);
      list.reserve(static_cast<std::size_t>(size));
      for (int i = 0; i < size; ++i) {
        list.push_back(Generate(depth - 1));
      }
      return Document(std::move(list));
    }
    DocumentMap map;
    const int size = Range(4);
    for (int i = 0; i < size; ++i) {
      map.insert_or_assign(RandomText() + std::to_string(i), Generate(depth - 1));
    }
    return Document(std::move(map));
  }

  std::mt19937_64 rng_;
  DocumentGeneratorOptions options_;
};

}  // namespace smithy::testing

#endif  // SMITHY_TESTS_TESTING_RANDOM_DOCUMENT_H_
