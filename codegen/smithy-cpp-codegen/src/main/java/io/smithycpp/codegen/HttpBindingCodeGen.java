package io.smithycpp.codegen;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.knowledge.HttpBinding;
import software.amazon.smithy.model.knowledge.HttpBindingIndex;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeType;
import software.amazon.smithy.model.traits.JsonNameTrait;
import software.amazon.smithy.model.traits.MediaTypeTrait;

/**
 * HTTP-binding emission shared by the client and server halves of the HTTP+JSON protocol (companion
 * to {@link SerdeCodeGen}, which owns the document-body expressions): binding partitioning,
 * the @jsonName wire key, @httpHeader and @httpPrefixHeaders read/write, @httpPayload read/write,
 * text-position value parsing, and the payload content-type predicates. Each read/write pair is its
 * own wire inverse — the client writes what the server reads and vice versa — which is why both
 * halves share these emitters.
 */
final class HttpBindingCodeGen {

  private HttpBindingCodeGen() {}

  /**
   * The JSON document key for a member: @jsonName when the module's protocol honors it
   * (ProtocolGenerator.usesJsonName), else the member name. The one policy shared by the serde
   * functions and the binding code — separate copies could give the two inconsistent body keys.
   */
  static String wireName(MemberShape member, boolean useJsonName) {
    if (!useJsonName) {
      return member.getMemberName();
    }
    return member
        .getTrait(JsonNameTrait.class)
        .map(JsonNameTrait::getValue)
        .orElse(member.getMemberName());
  }

  /** An operation's request bindings partitioned by location (maps sorted by location name). */
  record RequestBindings(
      Map<String, HttpBinding> labels,
      Map<String, HttpBinding> queries,
      Map<String, HttpBinding> headers,
      List<HttpBinding> body,
      HttpBinding queryParams,
      HttpBinding payload,
      HttpBinding prefixHeaders) {

    static RequestBindings of(HttpBindingIndex index, OperationShape operation) {
      return of(index, operation, null);
    }

    /**
     * Variant excluding one member — the event-stream union of a streaming operation (ADR-0016),
     * which is the session itself and never a wire binding of the upgrade request.
     */
    static RequestBindings of(
        HttpBindingIndex index, OperationShape operation, MemberShape excluded) {
      Map<String, HttpBinding> labels = new TreeMap<>();
      Map<String, HttpBinding> queries = new TreeMap<>();
      Map<String, HttpBinding> headers = new TreeMap<>();
      List<HttpBinding> body = new ArrayList<>();
      HttpBinding queryParams = null;
      HttpBinding payload = null;
      HttpBinding prefixHeaders = null;
      for (HttpBinding binding : index.getRequestBindings(operation).values()) {
        if (binding.getMember().equals(excluded)) {
          continue;
        }
        switch (binding.getLocation()) {
          case LABEL -> labels.put(binding.getLocationName(), binding);
          case QUERY -> queries.put(binding.getLocationName(), binding);
          case QUERY_PARAMS -> queryParams = binding;
          case HEADER -> headers.put(binding.getLocationName(), binding);
          case DOCUMENT -> body.add(binding);
          case PAYLOAD -> payload = binding;
          case PREFIX_HEADERS -> prefixHeaders = binding;
          default ->
              throw new CodegenException(
                  "cpp-codegen: HTTP+JSON request binding "
                      + binding.getLocation()
                      + " is not supported yet ("
                      + operation.getId()
                      + ")");
        }
      }
      return new RequestBindings(
          labels, queries, headers, body, queryParams, payload, prefixHeaders);
    }
  }

  /** An operation's response bindings partitioned by location (headers sorted by name). */
  record ResponseBindings(
      Map<String, HttpBinding> headers,
      List<HttpBinding> body,
      HttpBinding responseCode,
      HttpBinding payload,
      HttpBinding prefixHeaders) {

    static ResponseBindings of(HttpBindingIndex index, OperationShape operation) {
      Map<String, HttpBinding> headers = new TreeMap<>();
      List<HttpBinding> body = new ArrayList<>();
      HttpBinding responseCode = null;
      HttpBinding payload = null;
      HttpBinding prefixHeaders = null;
      for (HttpBinding binding : index.getResponseBindings(operation).values()) {
        switch (binding.getLocation()) {
          case DOCUMENT -> body.add(binding);
          case RESPONSE_CODE -> responseCode = binding;
          case PAYLOAD -> payload = binding;
          case PREFIX_HEADERS -> prefixHeaders = binding;
          case HEADER -> headers.put(binding.getLocationName(), binding);
          default ->
              throw new CodegenException(
                  "cpp-codegen: HTTP+JSON response binding "
                      + binding.getLocation()
                      + " is not supported yet ("
                      + operation.getId()
                      + ")");
        }
      }
      return new ResponseBindings(headers, body, responseCode, payload, prefixHeaders);
    }
  }

  /** Blob payloads without @mediaType accept any content type / accept header. */
  static boolean lenientPayload(CppContext context, OperationShape operation, boolean response) {
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    var bindings =
        response ? index.getResponseBindings(operation) : index.getRequestBindings(operation);
    return bindings.values().stream()
        .anyMatch(
            b -> {
              if (b.getLocation() != HttpBinding.Location.PAYLOAD) {
                return false;
              }
              Shape target = context.model().expectShape(b.getMember().getTarget());
              return target.isBlobShape()
                  && !target.hasTrait(MediaTypeTrait.class)
                  && !b.getMember().hasTrait(MediaTypeTrait.class);
            });
  }

  /**
   * Whether the payload member is a string/enum without @mediaType: simpleRestJson carries those as
   * JSON string values with content-type application/json (alloy's own conformance model pins this
   * — e.g. VersionOutput's body is {@code "1.0"}, quotes included), unlike @mediaType strings and
   * blobs, which stay raw.
   */
  static boolean jsonTextPayload(CppContext context, MemberShape member) {
    Shape target = context.model().expectShape(member.getTarget());
    return switch (target.getType()) {
      case STRING, ENUM ->
          !member.hasTrait(MediaTypeTrait.class) && !target.hasTrait(MediaTypeTrait.class);
      default -> false;
    };
  }

  /**
   * Writes each document-bound member of {@code owner} into an in-scope {@code body_map} (unset
   * optionals are skipped). Shared by the client request body and the server response body.
   */
  static void writeDocumentBodyMap(
      CppWriter w, SerdeCodeGen serde, List<HttpBinding> body, String owner) {
    for (HttpBinding binding : body) {
      serde.writeMemberSerialize(w, binding.getMember(), owner, "body_map");
    }
  }

  /** The message's content type when an @httpPayload member is bound (media-type aware). */
  static String payloadContentType(
      CppContext context, OperationShape operation, boolean isRequest) {
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    var bindings =
        isRequest ? index.getRequestBindings(operation) : index.getResponseBindings(operation);
    for (HttpBinding binding : bindings.values()) {
      if (binding.getLocation() == HttpBinding.Location.PAYLOAD
          && jsonTextPayload(context, binding.getMember())) {
        return "application/json";
      }
    }
    return isRequest
        ? index.determineRequestContentType(operation, "application/json").orElse("")
        : index.determineResponseContentType(operation, "application/json").orElse("");
  }

  /**
   * Writes the @httpPayload member of {@code in} as {@code messageVar}.body (+content-type). Shared
   * by the client request path and the server response path. Structure payloads always produce a
   * JSON document (at minimum "{}"); other unset optional payloads leave the body empty with no
   * content-type. A member-bound Content-Type header wins over the payload's.
   */
  static void writePayloadWrite(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      OperationShape operation,
      HttpBinding binding,
      String in,
      String messageVar,
      boolean isRequest) {
    MemberShape member = binding.getMember();
    Shape target = context.model().expectShape(member.getTarget());
    String field = in + "." + context.cppSymbols().toMemberName(member);
    boolean optional = !MemberDefaults.plain(context.model(), member);
    String value = optional ? "(*" + field + ")" : field;
    boolean structure = target.getType() == ShapeType.STRUCTURE;
    if (optional && !structure) {
      w.openBlock("if ($L.has_value()) {", field);
    }
    if (optional && structure) {
      w.openBlock("if ($L.has_value()) {", field);
      w.write(
          "$L.body = smithy::json::Encode($L);",
          messageVar,
          serde.serializeExpression(member, value));
      w.closeBlock("} else {");
      w.indent();
      w.write("$L.body = \"{}\";", messageVar);
      w.closeBlock("}");
    } else {
      boolean jsonText = jsonTextPayload(context, member);
      switch (target.getType()) {
        case BLOB -> w.write("$L.body = $L.ToString();", messageVar, value);
        case STRING -> {
          if (jsonText) {
            // simpleRestJson: plain string payloads are JSON string values.
            w.write("$L.body = smithy::json::Encode(smithy::Document($L));", messageVar, value);
          } else {
            w.write("$L.body = $L;", messageVar, value);
          }
        }
        case ENUM -> {
          if (jsonText) {
            w.write(
                "$L.body = smithy::json::Encode(smithy::Document(std::string($L.ToString())));",
                messageVar,
                value);
          } else {
            w.write("$L.body = std::string($L.ToString());", messageVar, value);
          }
        }
        default ->
            w.write(
                "$L.body = smithy::json::Encode($L);",
                messageVar,
                serde.serializeExpression(member, value));
      }
    }
    w.write(
        "if (!$L.headers.Get(\"content-type\").has_value()) $L.headers.Set(\"content-type\", $L);",
        messageVar,
        messageVar,
        CppLiterals.stringLiteral(payloadContentType(context, operation, isRequest)));
    if (optional && !structure) {
      w.closeBlock("}");
    }
    if (!isRequest) {
      // The suite asserts Content-Length on payload responses (incl. "0" for
      // an absent payload); document-body responses leave it to the transport.
      w.write(
          "$L.headers.Set(\"content-length\", std::to_string($L.body.size()));",
          messageVar,
          messageVar);
    }
  }

  /**
   * Parses a JSON document body member-by-member into {@code targetPrefix}<member> fields — the
   * READ direction both wire ends share (client response parse, server request parse), riding
   * {@link SerdeCodeGen#writeMemberRead} per member. The ends differ only in what a missing
   * required member means — clients fail the exchange, servers record a validation failure and keep
   * parsing — so that branch is the caller's {@code requiredAbsent} hook. Runs inside an
   * Outcome-returning function with an empty {@code structType} value already constructed.
   */
  static void writeDocumentBodyRead(
      CppWriter w,
      SerdeCodeGen serde,
      List<HttpBinding> body,
      String bodyExpr,
      String targetPrefix,
      String structType,
      String opName,
      SerdeCodeGen.RequiredAbsentEmitter requiredAbsent) {
    w.write("auto body_doc = smithy::json::Decode($L.empty() ? \"{}\" : $L);", bodyExpr, bodyExpr);
    w.write("if (!body_doc) return std::move(body_doc).error();");
    w.write(
        "if (!body_doc->is_map()) return smithy::Error::Serialization($S);",
        opName + ": expected a JSON object body");
    for (HttpBinding binding : body) {
      serde.writeMemberRead(
          w,
          binding.getMember(),
          "body_doc->",
          targetPrefix,
          structType,
          /* fillDefaults= */ false,
          requiredAbsent);
    }
  }

  /**
   * Reads a message body into the @httpPayload member at {@code targetPrefix}<member>. Shared by
   * the client response path and the server request path; an empty body leaves the member unset.
   * Runs inside an Outcome-returning function.
   */
  static void writePayloadRead(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      HttpBinding binding,
      String bodyExpr,
      String targetPrefix) {
    MemberShape member = binding.getMember();
    Shape target = context.model().expectShape(member.getTarget());
    String field = targetPrefix + context.cppSymbols().toMemberName(member);
    String path = member.getMemberName();
    boolean jsonText = jsonTextPayload(context, member);
    w.openBlock("if (!$L.empty()) {", bodyExpr);
    switch (target.getType()) {
      case BLOB -> w.write("$L = smithy::Blob::FromString($L);", field, bodyExpr);
      case STRING -> {
        if (jsonText) {
          // simpleRestJson: plain string payloads are JSON string values.
          w.write("auto payload_doc = smithy::json::Decode($L);", bodyExpr);
          w.write("if (!payload_doc) return std::move(payload_doc).error();");
          w.write(
              "if (!payload_doc->is_string()) return smithy::Error::Serialization("
                  + "\"expected a JSON string payload\");");
          w.write("$L = payload_doc->as_string();", field);
        } else {
          w.write("$L = $L;", field, bodyExpr);
        }
      }
      case ENUM -> {
        String enumType = context.cppSymbols().typeRef(target);
        if (jsonText) {
          w.write("auto payload_doc = smithy::json::Decode($L);", bodyExpr);
          w.write("if (!payload_doc) return std::move(payload_doc).error();");
          w.write(
              "if (!payload_doc->is_string()) return smithy::Error::Serialization("
                  + "\"expected a JSON string payload\");");
          w.write("$L = $L::FromString(payload_doc->as_string());", field, enumType);
        } else {
          w.write("$L = $L::FromString($L);", field, enumType, bodyExpr);
        }
      }
      default -> {
        w.write("auto payload_doc = smithy::json::Decode($L);", bodyExpr);
        w.write("if (!payload_doc) return std::move(payload_doc).error();");
        w.write("const smithy::Document* payload_ptr = &*payload_doc;");
        boolean structure = target.getType() == ShapeType.STRUCTURE;
        if (member.isRequired()) {
          serde.writeDeserializeInto(w, member, "payload_ptr", field, path);
        } else {
          if (structure) {
            // The inverse of writing "{}" for an unset structure payload:
            // an empty JSON object reads back as unset.
            w.openBlock("if (!payload_ptr->is_map() || !payload_ptr->as_map().empty()) {");
          }
          w.write("$L parsed_payload{};", context.cppSymbols().typeRef(target));
          serde.writeDeserializeInto(w, member, "payload_ptr", "parsed_payload", path);
          w.write("$L = std::move(parsed_payload);", field);
          if (structure) {
            w.closeBlock("}");
          }
        }
      }
    }
    w.closeBlock("}");
  }

  /** Writes an @httpPrefixHeaders map of {@code in} into {@code messageVar}.headers. */
  static void writePrefixHeadersWrite(
      CppWriter w, CppContext context, HttpBinding binding, String in, String messageVar) {
    MemberShape member = binding.getMember();
    String field = in + "." + context.cppSymbols().toMemberName(member);
    String prefix = binding.getLocationName();
    boolean optional = !MemberDefaults.plain(context.model(), member);
    String value = optional ? "(*" + field + ")" : field;
    if (optional) {
      w.openBlock("if ($L.has_value()) {", field);
    }
    w.openBlock("for (const auto& [map_key, map_value] : $L) {", value);
    w.write("$L.headers.Set($S + map_key, map_value);", messageVar, prefix);
    w.closeBlock("}");
    if (optional) {
      w.closeBlock("}");
    }
  }

  /**
   * Reads every header of {@code headersExpr} matching the binding's prefix (case-insensitively; an
   * empty prefix matches all) into the map member at {@code targetPrefix}<member>. No matching
   * headers leaves the member unset.
   */
  static void writePrefixHeadersRead(
      CppWriter w,
      CppContext context,
      HttpBinding binding,
      String headersExpr,
      String targetPrefix) {
    MemberShape member = binding.getMember();
    String field = targetPrefix + context.cppSymbols().toMemberName(member);
    String prefix = binding.getLocationName();
    boolean optional = !MemberDefaults.plain(context.model(), member);
    w.openBlock("for (const auto& [header_name, header_value] : $L.entries()) {", headersExpr);
    w.write("if (!smithy::http::HeaderNameStartsWith(header_name, $S)) continue;", prefix);
    String container;
    if (optional) {
      w.write("if (!$L.has_value()) $L.emplace();", field, field);
      container = "(*" + field + ")";
    } else {
      container = field;
    }
    w.write(
        "$L.insert_or_assign(header_name.substr($L), header_value);", container, prefix.length());
    w.closeBlock("}");
  }

  /**
   * Reads one @httpHeader binding from {@code headersExpr} into {@code targetPrefix}<member>
   * (client response headers and server request headers share this shape; failures return
   * smithy::Error, so it must run inside an Outcome-returning function).
   */
  static void writeHeaderReadBinding(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      HttpBinding binding,
      String headersExpr,
      String targetPrefix) {
    MemberShape member = binding.getMember();
    Shape target = context.model().expectShape(member.getTarget());
    String field = targetPrefix + context.cppSymbols().toMemberName(member);
    w.openBlock(
        "if (const auto header_value = $L.Get($S); header_value.has_value()) {",
        headersExpr,
        binding.getLocationName());
    if (target.isListShape()) {
      var list = target.asListShape().orElseThrow();
      MemberShape element = list.getMember();
      w.write("$L items;", context.cppSymbols().typeRef(list));
      String tsFormat = serde.timestampFormat(element, "smithy::TimestampFormat::kHttpDate");
      String splitter =
          context.model().expectShape(element.getTarget()).isTimestampShape()
                  && tsFormat.equals("smithy::TimestampFormat::kHttpDate")
              ? "SplitHttpDateHeaderValues"
              : "SplitHeaderListValues";
      w.openBlock("for (const std::string& part : smithy::http::$L(*header_value)) {", splitter);
      writeParsedHeaderValue(
          w, context, element, "part", "items.push_back", /* push= */ true, tsFormat);
      w.closeBlock("}");
      w.write("$L = std::move(items);", field);
    } else {
      writeParsedHeaderValue(
          w,
          context,
          member,
          "(*header_value)",
          field,
          /* push= */ false,
          serde.timestampFormat(member, "smithy::TimestampFormat::kHttpDate"));
    }
    w.closeBlock("}");
  }

  /**
   * Emits `<sink>(parsed)` / `<sink> = parsed;` for one header text value. Timestamps and
   * media-type strings parse through Outcomes, so those emit a statement pair instead of one
   * expression.
   */
  private static void writeParsedHeaderValue(
      CppWriter w,
      CppContext context,
      MemberShape member,
      String valueExpr,
      String sink,
      boolean push,
      String timestampFormat) {
    Shape target = context.model().expectShape(member.getTarget());
    String open = push ? sink + "(" : sink + " = ";
    String close = push ? ");" : ";";
    if (writeSimpleTextParse(w, context, target, valueExpr, open, close)) {
      return;
    }
    switch (target.getType()) {
      case TIMESTAMP -> {
        w.write("auto parsed_ts = smithy::Timestamp::Parse($L, $L);", valueExpr, timestampFormat);
        w.write("if (!parsed_ts) return std::move(parsed_ts).error();");
        w.write("$L*std::move(parsed_ts)$L", open, close);
      }
      case STRING -> {
        if (target.hasTrait(MediaTypeTrait.class)) {
          // restJson1: string headers with @mediaType are base64 encoded.
          w.write("auto decoded = smithy::Base64Decode($L);", valueExpr);
          w.write("if (!decoded) return std::move(decoded).error();");
          w.write("$Ldecoded->ToString()$L", open, close);
        } else {
          w.write("$L$L$L", open, valueExpr, close);
        }
      }
      default ->
          throw new CodegenException(
              "cpp-codegen: @httpHeader target " + target.getId() + " is not supported yet");
    }
  }

  /**
   * Emits statements converting a decoded text {@code valueExpr} in a label or query position into
   * {@code sink} (assignment target, or a push_back callee when {@code push}). Differs from the
   * header read only in timestamp strictness (servers reject non-Z RFC3339 offsets in text
   * positions) and string handling (no @mediaType base64 in labels/queries). Runs inside an
   * Outcome-returning function.
   */
  static void writeTextValueInto(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      MemberShape member,
      String valueExpr,
      String sink,
      String timestampDefault,
      boolean push) {
    Shape target = context.model().expectShape(member.getTarget());
    String open = push ? sink + "(" : sink + " = ";
    String close = push ? ");" : ";";
    if (writeSimpleTextParse(w, context, target, valueExpr, open, close)) {
      return;
    }
    switch (target.getType()) {
      case TIMESTAMP -> {
        String format = serde.timestampFormat(member, timestampDefault);
        if (format.equals("smithy::TimestampFormat::kDateTime")) {
          // Servers reject RFC3339 UTC offsets in text positions (clients must
          // accept them in responses, so Timestamp::Parse itself stays lenient).
          w.write(
              "if ($L.empty() || ($L.back() != 'Z' && $L.back() != 'z')) return "
                  + "smithy::Error::Serialization(\"expected a Z-terminated date-time\");",
              valueExpr,
              valueExpr,
              valueExpr);
        }
        w.write("auto parsed_ts = smithy::Timestamp::Parse($L, $L);", valueExpr, format);
        w.write("if (!parsed_ts) return std::move(parsed_ts).error();");
        w.write("$L*std::move(parsed_ts)$L", open, close);
      }
      case STRING -> w.write("$L$L$L", open, valueExpr, close);
      default ->
          throw new CodegenException(
              "cpp-codegen: label/query binding target " + target.getId() + " is not supported");
    }
  }

  /**
   * Emits the parse of one text-position value for the simple types whose emission is identical in
   * header, label, and query positions. Returns false for the position-dependent types (timestamps,
   * strings) so the caller supplies its own handling.
   */
  private static boolean writeSimpleTextParse(
      CppWriter w, CppContext context, Shape target, String valueExpr, String open, String close) {
    switch (target.getType()) {
      case ENUM ->
          w.write(
              "$L$L::FromString($L)$L",
              open,
              context.cppSymbols().typeRef(target),
              valueExpr,
              close);
      case BYTE, SHORT, INTEGER, LONG, INT_ENUM -> {
        w.write(
            "auto parsed_num = helpers::ParseInt64Text($L, $L);",
            valueExpr,
            ProtocolSupport.int64Bounds(target.getType()));
        w.write("if (!parsed_num) return std::move(parsed_num).error();");
        w.write(
            "$Lstatic_cast<$L>(*parsed_num)$L", open, context.cppSymbols().typeRef(target), close);
      }
      case FLOAT -> {
        // Checked narrowing: a finite text value beyond float range would be
        // UB to cast (UBSan float-cast-overflow), so it fails the parse.
        w.write("auto parsed_num = helpers::ParseDoubleText($L);", valueExpr);
        w.write("if (!parsed_num) return std::move(parsed_num).error();");
        w.write("auto narrowed_num = smithy::FloatFromDouble(*parsed_num);");
        w.write("if (!narrowed_num) return std::move(narrowed_num).error();");
        w.write("$L*narrowed_num$L", open, close);
      }
      case DOUBLE -> {
        w.write("auto parsed_num = helpers::ParseDoubleText($L);", valueExpr);
        w.write("if (!parsed_num) return std::move(parsed_num).error();");
        w.write("$L*parsed_num$L", open, close);
      }
      case BOOLEAN -> {
        w.write(
            "if ($L != \"true\" && $L != \"false\") return "
                + "smithy::Error::Serialization(\"expected a boolean, got: \" + $L);",
            valueExpr,
            valueExpr,
            valueExpr);
        w.write("$L($L == \"true\")$L", open, valueExpr, close);
      }
      default -> {
        return false;
      }
    }
    return true;
  }

  /** Writes one @httpHeader binding from {@code owner}.<member> into {@code headersExpr}. */
  static void writeHeaderWriteBinding(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      HttpBinding binding,
      String owner,
      String headersExpr) {
    MemberShape member = binding.getMember();
    Shape target = context.model().expectShape(member.getTarget());
    String field = owner + "." + context.cppSymbols().toMemberName(member);
    String name = binding.getLocationName();
    boolean plain = MemberDefaults.plain(context.model(), member);
    String access = plain ? field : "(*" + field + ")";
    if (!plain) {
      w.openBlock("if ($L.has_value()) {", field);
    }
    if (target.isListShape()) {
      MemberShape element = target.asListShape().orElseThrow().getMember();
      w.openBlock("{");
      w.write("std::string joined;");
      w.openBlock("for (const auto& item : $L) {", access);
      w.write("if (!joined.empty()) joined += \", \";");
      w.write(
          "joined += $L;",
          ProtocolSupport.toStringExpression(
              context,
              element,
              "item",
              serde.timestampFormat(element, "smithy::TimestampFormat::kHttpDate")));
      w.closeBlock("}");
      w.write("$L.Set($S, joined);", headersExpr, name);
      w.closeBlock("}");
    } else {
      String expr =
          ProtocolSupport.toStringExpression(
              context,
              member,
              access,
              serde.timestampFormat(member, "smithy::TimestampFormat::kHttpDate"));
      if (target.isStringShape() && target.hasTrait(MediaTypeTrait.class)) {
        // restJson1: string headers with @mediaType are base64 encoded.
        expr = "smithy::Base64Encode(smithy::Blob::FromString(" + expr + "))";
      }
      w.write("$L.Set($S, $L);", headersExpr, name, expr);
    }
    if (!plain) {
      w.closeBlock("}");
    }
  }
}
