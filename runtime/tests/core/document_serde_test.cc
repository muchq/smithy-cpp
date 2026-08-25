#include "smithy/core/document_serde.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "smithy/core/uuid.h"

namespace smithy {
namespace {

TEST(DocumentSerdeTest, TimestampFromAllWireShapes) {
  const auto ts = Timestamp::FromEpochMilliseconds(1398796238500);

  // CBOR path: typed node, format irrelevant.
  const Document node = Document::FromTimestamp(ts, TimestampFormat::kEpochSeconds);
  ASSERT_TRUE(TimestampFromDocument(node, TimestampFormat::kDateTime).ok());
  EXPECT_EQ(*TimestampFromDocument(node, TimestampFormat::kEpochSeconds), ts);

  // JSON epoch-seconds: number.
  EXPECT_EQ(*TimestampFromDocument(Document(1398796238.5), TimestampFormat::kEpochSeconds), ts);
  EXPECT_EQ(
      *TimestampFromDocument(Document(std::int64_t{1398796238}), TimestampFormat::kEpochSeconds),
      Timestamp::FromEpochMilliseconds(1398796238000));

  // String formats.
  EXPECT_EQ(*TimestampFromDocument(Document("2014-04-29T18:30:38.5Z"), TimestampFormat::kDateTime),
            ts);
  EXPECT_EQ(
      *TimestampFromDocument(Document("Tue, 29 Apr 2014 18:30:38 GMT"), TimestampFormat::kHttpDate),
      Timestamp::FromEpochMilliseconds(1398796238000));
}

TEST(DocumentSerdeTest, TimestampRejectsMismatchedShapes) {
  EXPECT_FALSE(TimestampFromDocument(Document("123"), TimestampFormat::kEpochSeconds).ok());
  EXPECT_FALSE(TimestampFromDocument(Document(5), TimestampFormat::kDateTime).ok());
  EXPECT_FALSE(TimestampFromDocument(Document(true), TimestampFormat::kEpochSeconds).ok());
  EXPECT_FALSE(TimestampFromDocument(Document("garbage"), TimestampFormat::kDateTime).ok());
}

TEST(DocumentSerdeTest, BlobFromNodeAndBase64) {
  const Blob blob = Blob::FromString("foobar");
  EXPECT_EQ(*BlobFromDocument(Document(blob)), blob);
  EXPECT_EQ(*BlobFromDocument(Document("Zm9vYmFy")), blob);
  EXPECT_FALSE(BlobFromDocument(Document("not base64!")).ok());
  EXPECT_FALSE(BlobFromDocument(Document(7)).ok());
}

TEST(UuidTest, GeneratesCanonicalV4) {
  const std::string uuid = GenerateUuidV4();
  ASSERT_EQ(uuid.size(), 36u);
  EXPECT_EQ(uuid[8], '-');
  EXPECT_EQ(uuid[13], '-');
  EXPECT_EQ(uuid[18], '-');
  EXPECT_EQ(uuid[23], '-');
  EXPECT_EQ(uuid[14], '4');  // version nibble
  const char variant = uuid[19];
  EXPECT_TRUE(variant == '8' || variant == '9' || variant == 'a' || variant == 'b') << uuid;
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    const char c = uuid[i];
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << uuid;
  }
}

TEST(UuidTest, GeneratesDistinctValues) {
  const std::string a = GenerateUuidV4();
  const std::string b = GenerateUuidV4();
  EXPECT_NE(a, b);
}

}  // namespace
TEST(DocumentSerdeTest, DoubleFromDocumentAcceptsNonFiniteSpellings) {
  EXPECT_EQ(*DoubleFromDocument(Document(1.5)), 1.5);
  EXPECT_EQ(*DoubleFromDocument(Document(std::int64_t{2})), 2.0);
  EXPECT_TRUE(std::isnan(*DoubleFromDocument(Document(std::string("NaN")))));
  EXPECT_EQ(*DoubleFromDocument(Document(std::string("Infinity"))),
            std::numeric_limits<double>::infinity());
  EXPECT_EQ(*DoubleFromDocument(Document(std::string("-Infinity"))),
            -std::numeric_limits<double>::infinity());
  EXPECT_FALSE(DoubleFromDocument(Document(std::string("other"))).ok());
  EXPECT_FALSE(DoubleFromDocument(Document(true)).ok());
}

TEST(DocumentSerdeTest, FloatFromDoubleNarrowsInRangeValues) {
  EXPECT_EQ(*FloatFromDouble(1.5), 1.5F);
  EXPECT_EQ(*FloatFromDouble(-2.25), -2.25F);
  EXPECT_EQ(*FloatFromDouble(0.0), 0.0F);
  // The exact float extremes are in range, not rejected.
  EXPECT_EQ(*FloatFromDouble(std::numeric_limits<float>::max()), std::numeric_limits<float>::max());
  EXPECT_EQ(*FloatFromDouble(-std::numeric_limits<float>::max()),
            -std::numeric_limits<float>::max());
  // Values below float precision round (here: to zero) — rounding is not an
  // error, only magnitude overflow is.
  EXPECT_EQ(*FloatFromDouble(1e-300), 0.0F);
}

TEST(DocumentSerdeTest, FloatFromDoubleRejectsFiniteOverflow) {
  // The raw static_cast would be UB for these ([conv.double]); a hostile
  // request body carrying 1e300 for a float member must fail the parse.
  EXPECT_FALSE(FloatFromDouble(1e300).ok());
  EXPECT_FALSE(FloatFromDouble(-1e300).ok());
  EXPECT_FALSE(FloatFromDouble(std::numeric_limits<double>::max()).ok());
  // Just past the float edge in double precision is already overflow.
  EXPECT_FALSE(
      FloatFromDouble(std::nextafter(static_cast<double>(std::numeric_limits<float>::max()),
                                     std::numeric_limits<double>::infinity()))
          .ok());
}

TEST(DocumentSerdeTest, FloatFromDoublePassesNonFiniteThrough) {
  // Smithy float carries NaN/±Infinity on every wire; narrowing them is
  // well-defined and must not be caught in the overflow net.
  EXPECT_TRUE(std::isnan(*FloatFromDouble(std::numeric_limits<double>::quiet_NaN())));
  EXPECT_EQ(*FloatFromDouble(std::numeric_limits<double>::infinity()),
            std::numeric_limits<float>::infinity());
  EXPECT_EQ(*FloatFromDouble(-std::numeric_limits<double>::infinity()),
            -std::numeric_limits<float>::infinity());
}

TEST(DocumentSerdeTest, FormatFloatingPoint) {
  EXPECT_EQ(FormatDouble(4.1), "4.1");
  EXPECT_EQ(FormatFloat(4.1F), "4.1");
  EXPECT_EQ(FormatDouble(5.0), "5.0");
  EXPECT_EQ(FormatDouble(std::numeric_limits<double>::quiet_NaN()), "NaN");
  EXPECT_EQ(FormatDouble(std::numeric_limits<double>::infinity()), "Infinity");
  EXPECT_EQ(FormatFloat(-std::numeric_limits<float>::infinity()), "-Infinity");
}

}  // namespace smithy
