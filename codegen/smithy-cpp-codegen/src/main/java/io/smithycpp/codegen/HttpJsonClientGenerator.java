package io.smithycpp.codegen;

import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.model.knowledge.HttpBinding;
import software.amazon.smithy.model.knowledge.HttpBindingIndex;
import software.amazon.smithy.model.pattern.SmithyPattern;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.HttpTrait;
import software.amazon.smithy.model.traits.MediaTypeTrait;

/**
 * The client half of the HTTP+JSON protocol: one operation-method body per operation — request
 * target/query/headers/body from the @http bindings, response status check, then response
 * body/header/payload extraction — plus the client-side error support helpers. Shared read/write
 * emission lives in {@link HttpBindingCodeGen}; {@link HttpJsonServerGenerator} emits the mirror
 * image of each wire step.
 */
final class HttpJsonClientGenerator {

  /**
   * The response header carrying modeled-error identity (the error shape name), or "" when the
   * protocol carries no error header. simpleRestJson uses {@code x-error-type}.
   */
  private final String errorTypeHeaderName;

  /** Mirror of the owning protocol's usesJsonName() — body keys must match the serde functions. */
  private final boolean useJsonName;

  HttpJsonClientGenerator(String errorTypeHeaderName, boolean useJsonName) {
    this.errorTypeHeaderName = errorTypeHeaderName;
    this.useJsonName = useJsonName;
  }

  List<String> includes() {
    return List.of(
        "\"smithy/json/json.h\"",
        "\"smithy/core/base64.h\"",
        "\"smithy/http/headers.h\"",
        "<cstdint>",
        "<cstdlib>",
        "<limits>");
  }

  void writeHelpers(CppWriter w) {
    ProtocolSupport.writeErrorSupport(
        w, "auto doc = smithy::json::Decode(response.body);", errorTypeHeaderName);
    ProtocolSupport.writeNumericParseHelpers(w);
  }

  void writeErrorDocPatches(CppWriter w, CppContext context, StructureShape error) {
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    for (HttpBinding binding : new TreeMap<>(index.getResponseBindings(error)).values()) {
      if (binding.getLocation() != HttpBinding.Location.HEADER) {
        continue;
      }
      MemberShape member = binding.getMember();
      Shape target = context.model().expectShape(member.getTarget());
      // Outcome-parsing kinds (timestamps, media-type strings) and lists are
      // not patched into error documents yet; the exclusion list documents
      // any affected conformance cases.
      switch (target.getType()) {
        case STRING -> {
          if (target.hasTrait(MediaTypeTrait.class)) {
            continue;
          }
        }
        case ENUM, BYTE, SHORT, INTEGER, LONG, INT_ENUM, FLOAT, DOUBLE, BOOLEAN -> {}
        default -> {
          continue;
        }
      }
      // The document key is what the serde reads.
      String wireName = HttpBindingCodeGen.wireName(member, useJsonName);
      w.openBlock(
          "if (const auto header_value = response.headers.Get($S); header_value.has_value()) {",
          binding.getLocationName());
      switch (target.getType()) {
        case STRING, ENUM ->
            w.write(
                "parsed.doc.as_map().insert_or_assign($S, smithy::Document(*header_value));",
                wireName);
        // Document patching is best effort: malformed header values are
        // skipped, never failures.
        case BYTE, SHORT, INTEGER, LONG, INT_ENUM ->
            w.write(
                "if (auto parsed_num = helpers::ParseInt64Text(*header_value, $L)) "
                    + "parsed.doc.as_map().insert_or_assign($S, smithy::Document(*parsed_num));",
                ProtocolSupport.int64Bounds(target.getType()),
                wireName);
        case FLOAT, DOUBLE ->
            w.write(
                "if (auto parsed_num = helpers::ParseDoubleText(*header_value)) "
                    + "parsed.doc.as_map().insert_or_assign($S, smithy::Document(*parsed_num));",
                wireName);
        case BOOLEAN ->
            w.write(
                "parsed.doc.as_map().insert_or_assign($S, "
                    + "smithy::Document(*header_value == \"true\"));",
                wireName);
        default ->
            throw new CodegenException(
                "cpp-codegen: error @httpHeader doc patch for "
                    + target.getType()
                    + " is unreachable ("
                    + member.getId()
                    + ")");
      }
      w.closeBlock("}");
    }
  }

  void writeOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    HttpTrait http =
        operation
            .getTrait(HttpTrait.class)
            .orElseThrow(
                () ->
                    new CodegenException(
                        "cpp-codegen: HTTP+JSON operation missing @http: " + operation.getId()));
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StructureShape output = ProtocolSupport.outputShape(context, operation);
    SerdeCodeGen serde = new SerdeCodeGen(context, useJsonName);
    HttpBindingCodeGen.RequestBindings req =
        HttpBindingCodeGen.RequestBindings.of(index, operation);
    HttpBindingCodeGen.ResponseBindings resp =
        HttpBindingCodeGen.ResponseBindings.of(index, operation);
    Map<String, HttpBinding> labels = req.labels();
    Map<String, HttpBinding> queries = req.queries();
    Map<String, HttpBinding> headers = req.headers();
    List<HttpBinding> body = req.body();
    HttpBinding queryParams = req.queryParams();
    HttpBinding payload = req.payload();
    HttpBinding prefixHeaders = req.prefixHeaders();
    Map<String, HttpBinding> responseHeaders = resp.headers();
    List<HttpBinding> responseBody = resp.body();
    HttpBinding responseCode = resp.responseCode();
    HttpBinding responsePayload = resp.payload();
    HttpBinding responsePrefixHeaders = resp.prefixHeaders();

    String in =
        ProtocolSupport.prepareIdempotencyTokens(
            w, context, input, context.cppSymbols().toSymbol(input).getName());

    writeTarget(w, context, serde, operation, http, labels, queries, queryParams, in);

    w.write("smithy::http::HttpRequest request;");
    w.write("request.method = $S;", http.getMethod());
    w.write("request.target = std::move(target);");
    for (HttpBinding binding : headers.values()) {
      HttpBindingCodeGen.writeHeaderWriteBinding(w, context, serde, binding, in, "request.headers");
    }
    if (prefixHeaders != null) {
      HttpBindingCodeGen.writePrefixHeadersWrite(w, context, prefixHeaders, in, "request");
    }

    if (payload != null) {
      HttpBindingCodeGen.writePayloadWrite(
          w, context, serde, operation, payload, in, "request", true);
    } else if (!body.isEmpty()) {
      w.write("smithy::DocumentMap body_map;");
      HttpBindingCodeGen.writeDocumentBodyMap(w, serde, body, in);
      w.write("request.body = smithy::json::Encode(smithy::Document(std::move(body_map)));");
      w.write("request.headers.Set(\"content-type\", \"application/json\");");
    }

    if (responsePayload != null) {
      w.write(
          "request.headers.Set(\"accept\", $S);",
          HttpBindingCodeGen.payloadContentType(context, operation, false));
    }
    ProtocolSupport.writeRequestCompression(w, operation);
    w.write("auto response = Send(std::move(request));");
    w.write("if (!response) return std::move(response).error();");
    if (responseCode != null) {
      // The service chooses the status at runtime via @httpResponseCode, so
      // there is no modeled code to compare against: success is anything that
      // is not an error status. 3xx counts — a modeled redirect binds Location
      // and carries no error body — and the static branch below already
      // accepts one, so rejecting it here was an inconsistency, not a policy.
      w.write(
          "if (response->status < 200 || response->status >= 400) return $L;",
          ProtocolSupport.errorExpression(context, service, operation));
    } else {
      w.write(
          "if (response->status != $L) return $L;",
          http.getCode(),
          ProtocolSupport.errorExpression(context, service, operation));
    }

    if (ProtocolSupport.writeEmptyOutputReturn(w, context, output)) {
      return;
    }
    String outType = context.cppSymbols().toSymbol(output).getName();
    boolean allOptional = output.members().stream().noneMatch(MemberShape::isRequired);
    if (responseHeaders.isEmpty()
        && responseCode == null
        && responsePayload == null
        && responsePrefixHeaders == null) {
      if (allOptional) {
        w.write("if (response->body.empty()) return $L{};", outType);
      }
      w.write("auto body_doc = smithy::json::Decode(response->body);");
      w.write("if (!body_doc) return std::move(body_doc).error();");
      w.write(
          "return Deserialize$L(*body_doc);", SerdeCodeGen.serdeFunctionSuffix(context, output));
      return;
    }
    w.write("$L out{};", outType);
    // The whole-shape deserializer enforces every required member, so it only
    // fits when required members all travel in the body; a required member
    // bound elsewhere (header/@httpResponseCode) forces a member-by-member
    // body parse (mirroring the server's input parse).
    boolean requiredOutsideBody =
        responseHeaders.values().stream().anyMatch(b -> b.getMember().isRequired())
            || (responseCode != null && responseCode.getMember().isRequired())
            || (responsePrefixHeaders != null && responsePrefixHeaders.getMember().isRequired());
    if (!responseBody.isEmpty() && !requiredOutsideBody) {
      // A required member here means a required body member: parse regardless.
      if (allOptional) {
        w.openBlock("if (!response->body.empty()) {");
      }
      w.write("auto body_doc = smithy::json::Decode(response->body);");
      w.write("if (!body_doc) return std::move(body_doc).error();");
      w.write(
          "auto parsed = Deserialize$L(*body_doc);",
          SerdeCodeGen.serdeFunctionSuffix(context, output));
      w.write("if (!parsed) return std::move(parsed).error();");
      w.write("out = *std::move(parsed);");
      if (allOptional) {
        w.closeBlock("}");
      }
    } else if (!responseBody.isEmpty()) {
      HttpBindingCodeGen.writeDocumentBodyRead(
          w,
          serde,
          responseBody,
          "response->body",
          "out.",
          outType,
          CppReservedWords.escape(operation.getId().getName()),
          (w2, member, deserializeMember) -> {
            // Clients are strict: a missing required member fails the exchange.
            w2.write(
                "if ($L) return smithy::Error::Serialization($S);",
                SerdeCodeGen.MEMBER_ABSENT,
                "missing required member: " + member.getMemberName());
            deserializeMember.run();
          });
    }
    if (responsePayload != null) {
      HttpBindingCodeGen.writePayloadRead(
          w, context, serde, responsePayload, "response->body", "out.");
    }
    for (HttpBinding binding : responseHeaders.values()) {
      HttpBindingCodeGen.writeHeaderReadBinding(
          w, context, serde, binding, "response->headers", "out.");
      if (binding.getMember().isRequired()) {
        // Clients are strict about required headers, like required body members.
        w.write(
            "if (!response->headers.Get($S).has_value()) return "
                + "smithy::Error::Serialization($S);",
            binding.getLocationName(),
            "missing required header: " + binding.getLocationName());
      }
    }
    if (responsePrefixHeaders != null) {
      HttpBindingCodeGen.writePrefixHeadersRead(
          w, context, responsePrefixHeaders, "response->headers", "out.");
    }
    if (responseCode != null) {
      MemberShape codeMember = responseCode.getMember();
      String type =
          context
              .cppSymbols()
              .toSymbol(context.model().expectShape(codeMember.getTarget()))
              .getName();
      w.write(
          "out.$L = static_cast<$L>(response->status);",
          context.cppSymbols().toMemberName(codeMember),
          type);
    }
    w.write("return out;");
  }

  /**
   * The request target — path segments in pattern order, then query — into a {@code target} local.
   * Shared by the unary request and the streaming upgrade request (ADR-0016), so label/query
   * resolution cannot drift between them.
   */
  private void writeTarget(
      CppWriter w,
      CppContext context,
      SerdeCodeGen serde,
      OperationShape operation,
      HttpTrait http,
      Map<String, HttpBinding> labels,
      Map<String, HttpBinding> queries,
      HttpBinding queryParams,
      String in) {
    w.write("std::string target = path_prefix_;");
    for (SmithyPattern.Segment segment : http.getUri().getSegments()) {
      if (!segment.isLabel()) {
        w.write("target += \"/$L\";", segment.getContent());
        continue;
      }
      HttpBinding binding = labels.get(segment.getContent());
      if (binding == null) {
        throw new CodegenException(
            "cpp-codegen: no @httpLabel member for {"
                + segment.getContent()
                + "} in "
                + operation.getId());
      }
      String value =
          ProtocolSupport.toStringExpression(
              context,
              binding.getMember(),
              in + "." + context.cppSymbols().toMemberName(binding.getMember()),
              serde.timestampFormat(binding.getMember(), "smithy::TimestampFormat::kDateTime"));
      String encoder = segment.isGreedyLabel() ? "EncodeGreedyPathSegment" : "EncodePathSegment";
      w.write("target += \"/\";");
      w.write("target += smithy::http::$L($L);", encoder, value);
    }
    boolean hasQuery =
        !queries.isEmpty() || queryParams != null || !http.getUri().getQueryLiterals().isEmpty();
    if (hasQuery) {
      w.write("smithy::http::QueryString query;");
      for (Map.Entry<String, String> literal :
          new TreeMap<>(http.getUri().getQueryLiterals()).entrySet()) {
        if (literal.getValue().isEmpty()) {
          w.write("query.AddFlag($S);", literal.getKey());
        } else {
          w.write("query.Add($S, $S);", literal.getKey(), literal.getValue());
        }
      }
      for (HttpBinding binding : queries.values()) {
        writeQueryBinding(w, context, serde, binding, in);
      }
      if (queryParams != null) {
        writeQueryParamsBinding(w, context, queryParams, in);
      }
      w.write("target += query.ToString();");
    }
  }

  /**
   * The streaming operation body (ADR-0016): the upgrade request from the same @http bindings a
   * unary request resolves — labels and query onto the target, headers onto the upgrade GET; the
   * stream member is the session, never a binding (and validate() rejected everything else) — then
   * the shared dial-and-wrap tail.
   */
  void writeStreamingOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    HttpTrait http =
        operation
            .getTrait(HttpTrait.class)
            .orElseThrow(
                () ->
                    new CodegenException(
                        "cpp-codegen: HTTP+JSON operation missing @http: " + operation.getId()));
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    SerdeCodeGen serde = new SerdeCodeGen(context, useJsonName);
    HttpBindingCodeGen.RequestBindings req =
        HttpBindingCodeGen.RequestBindings.of(index, operation);
    boolean usesInput =
        !req.labels().isEmpty()
            || !req.queries().isEmpty()
            || req.queryParams() != null
            || !req.headers().isEmpty()
            || req.prefixHeaders() != null;
    if (!usesInput && !input.members().isEmpty()) {
      w.write("(void)input;");
    }
    String in =
        ProtocolSupport.prepareIdempotencyTokens(
            w, context, input, context.cppSymbols().toSymbol(input).getName());
    writeTarget(
        w, context, serde, operation, http, req.labels(), req.queries(), req.queryParams(), in);
    w.write("smithy::http::WebSocketDialRequest request;");
    w.write("request.target = std::move(target);");
    for (HttpBinding binding : req.headers().values()) {
      HttpBindingCodeGen.writeHeaderWriteBinding(w, context, serde, binding, in, "request.headers");
    }
    if (req.prefixHeaders() != null) {
      HttpBindingCodeGen.writePrefixHeadersWrite(w, context, req.prefixHeaders(), in, "request");
    }
    w.write("request.headers.Set(\"user-agent\", config_.user_agent);");
    ClientGenerator.writeAuth(w, service);
    EventStreamCodeGen.writeDialAndReturn(w, context, operation);
  }

  /**
   * @httpQuery member: simple value, or a list of simple values added one entry per element.
   */
  private void writeQueryBinding(
      CppWriter w, CppContext context, SerdeCodeGen serde, HttpBinding binding, String in) {
    MemberShape member = binding.getMember();
    Shape target = context.model().expectShape(member.getTarget());
    String field = in + "." + context.cppSymbols().toMemberName(member);
    String name = binding.getLocationName();
    boolean plain = MemberDefaults.plain(context.model(), member);
    String access = plain ? field : "(*" + field + ")";
    if (!plain) {
      w.openBlock("if ($L.has_value()) {", field);
    }
    if (target.isListShape()) {
      MemberShape element = target.asListShape().orElseThrow().getMember();
      w.openBlock("for (const auto& item : $L) {", access);
      w.write(
          "query.Add($S, $L);",
          name,
          ProtocolSupport.toStringExpression(
              context,
              element,
              "item",
              serde.timestampFormat(element, "smithy::TimestampFormat::kDateTime")));
      w.closeBlock("}");
    } else {
      w.write(
          "query.Add($S, $L);",
          name,
          ProtocolSupport.toStringExpression(
              context,
              member,
              access,
              serde.timestampFormat(member, "smithy::TimestampFormat::kDateTime")));
    }
    if (!plain) {
      w.closeBlock("}");
    }
  }

  /**
   * @httpQueryParams map: bound query members and literals take precedence (query.Has).
   */
  private void writeQueryParamsBinding(
      CppWriter w, CppContext context, HttpBinding binding, String in) {
    MemberShape member = binding.getMember();
    var map = context.model().expectShape(member.getTarget()).asMapShape().orElseThrow();
    Shape valueTarget = context.model().expectShape(map.getValue().getTarget());
    String field = in + "." + context.cppSymbols().toMemberName(member);
    boolean plain = MemberDefaults.plain(context.model(), member);
    String access = plain ? field : "(*" + field + ")";
    if (!plain) {
      w.openBlock("if ($L.has_value()) {", field);
    }
    w.openBlock("for (const auto& [key, value] : $L) {", access);
    w.write("if (query.Has(key)) continue;");
    if (valueTarget.isListShape()) {
      w.openBlock("for (const auto& item : value) {");
      w.write("query.Add(key, item);");
      w.closeBlock("}");
    } else {
      w.write("query.Add(key, value);");
    }
    w.closeBlock("}");
    if (!plain) {
      w.closeBlock("}");
    }
  }
}
