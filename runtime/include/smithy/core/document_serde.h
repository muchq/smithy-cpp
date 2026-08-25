#ifndef SMITHY_CORE_DOCUMENT_SERDE_H_
#define SMITHY_CORE_DOCUMENT_SERDE_H_

#include "smithy/core/blob.h"
#include "smithy/core/document.h"
#include "smithy/core/outcome.h"
#include "smithy/core/timestamp.h"

namespace smithy {

// Helpers used by generated deserializers to read protocol-shaped Document
// nodes back into typed values. They are wire-format tolerant: a JSON decode
// yields numbers/strings while a CBOR decode yields typed nodes, and the same
// generated code must accept both.

// Accepts a timestamp node (CBOR tag 1), a number (epoch-seconds), or a
// string in the given format.
Outcome<Timestamp> TimestampFromDocument(const Document& doc, TimestampFormat format);

// Accepts a blob node (CBOR byte string) or a base64 string (JSON).
Outcome<Blob> BlobFromDocument(const Document& doc);

// Accepts a number, or the Smithy JSON non-finite encodings "NaN",
// "Infinity", "-Infinity".
Outcome<double> DoubleFromDocument(const Document& doc);

// Checked double→float narrowing for generated float members: a finite value
// beyond float range is rejected (the raw cast would be undefined behavior),
// NaN and ±Infinity pass through with their float spellings.
Outcome<float> FloatFromDouble(double value);

// Shortest round-trip decimal text (std::to_chars); non-finite values render
// as the Smithy wire spellings "NaN" / "Infinity" / "-Infinity". Used for
// HTTP label/query/header bindings and JSON bodies.
std::string FormatDouble(double value);
std::string FormatFloat(float value);

}  // namespace smithy

#endif  // SMITHY_CORE_DOCUMENT_SERDE_H_
