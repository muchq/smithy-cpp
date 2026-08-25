#include "smithy/core/document_serde.h"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <system_error>

#include "smithy/core/base64.h"

namespace smithy {

Outcome<Timestamp> TimestampFromDocument(const Document& doc, TimestampFormat format) {
  if (doc.is_timestamp()) {
    return doc.as_timestamp().value;
  }
  if (doc.is_int() || doc.is_double()) {
    if (format != TimestampFormat::kEpochSeconds) {
      return Error::Serialization("timestamp: numeric value for a string-formatted timestamp");
    }
    return Timestamp::FromEpochSecondsChecked(doc.AsNumber());
  }
  if (doc.is_string()) {
    if (format == TimestampFormat::kEpochSeconds) {
      return Error::Serialization("timestamp: string value for an epoch-seconds timestamp");
    }
    return Timestamp::Parse(doc.as_string(), format);
  }
  return Error::Serialization("timestamp: expected a timestamp, number, or string");
}

Outcome<Blob> BlobFromDocument(const Document& doc) {
  if (doc.is_blob()) {
    return doc.as_blob();
  }
  if (doc.is_string()) {
    return Base64Decode(doc.as_string());
  }
  return Error::Serialization("blob: expected a byte string or base64 text");
}

Outcome<double> DoubleFromDocument(const Document& doc) {
  if (doc.is_int() || doc.is_double()) {
    return doc.AsNumber();
  }
  if (doc.is_string()) {
    const std::string& text = doc.as_string();
    if (text == "NaN") return std::nan("");
    if (text == "Infinity") return std::numeric_limits<double>::infinity();
    if (text == "-Infinity") return -std::numeric_limits<double>::infinity();
  }
  return Error::Serialization("number: expected a number or NaN/Infinity/-Infinity");
}

Outcome<float> FloatFromDouble(double value) {
  // Only finite out-of-range values are rejected: casting those is undefined
  // behavior ([conv.double]), while NaN and ±Infinity narrow losslessly and
  // are legal Smithy float values on every wire.
  if (std::isfinite(value) &&
      (value < -std::numeric_limits<float>::max() || value > std::numeric_limits<float>::max())) {
    return Error::Serialization("number: value out of float range");
  }
  return static_cast<float>(value);
}

namespace {
template <typename T>
std::string FormatFloating(T value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
  std::array<char, 64> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  std::string text(buffer.data(), result.ptr);
  // to_chars omits ".0" for integral values; keep it so the text stays
  // unambiguously floating point ("5" -> "5.0").
  if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
      text.find("inf") == std::string::npos && text.find("nan") == std::string::npos) {
    text += ".0";
  }
  return text;
}
}  // namespace

std::string FormatDouble(double value) { return FormatFloating(value); }
std::string FormatFloat(float value) { return FormatFloating(value); }

}  // namespace smithy
