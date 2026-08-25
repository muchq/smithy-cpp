package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.ShapeId;
import software.amazon.smithy.model.shapes.StructureShape;

/**
 * smithy.cpp.protocols#jsonRpc2: the homegrown vendor-neutral JSON-RPC 2.0 protocol. A single
 * {@code POST /} endpoint carries {@code {"jsonrpc":"2.0","method":<Operation>,"params":<input>,
 * "id":…}}; responses are {@code {"jsonrpc":"2.0","result":<output>,"id":…}} or a JSON-RPC error
 * object over the shared {@code smithy::Document} JSON pivot. No HTTP bindings apply.
 *
 * <p>Wire contract (this class is the normative definition; the authored conformance suite under
 * {@code protocol-tests/jsonrpc2} pins it):
 *
 * <ul>
 *   <li>Every JSON-RPC response — success or error — travels as HTTP 200 with content-type {@code
 *       application/json}. Non-200 statuses only arise below the protocol (router 404s, proxies)
 *       and surface client-side as unknown errors.
 *   <li>The client always sends {@code "id": 1}: the HTTP transport is synchronous
 *       request/response, so a constant id is unambiguous (and keeps conformance bodies
 *       deterministic). The server echoes the request id, or {@code null} when the envelope never
 *       yielded one (JSON-RPC 2.0 §5).
 *   <li>Operations whose input is {@code smithy.api#Unit} omit {@code params}; the server treats an
 *       absent or null {@code params} as an empty object.
 *   <li>Error envelopes: {@code error.code} is the modeled error's {@code @httpError} status (or
 *       the 400/500 {@code @error} class default) — an application-defined code outside the
 *       reserved JSON-RPC range that preserves HTTP retryability semantics. Validation failures use
 *       400 with {@code smithy.framework#ValidationException} identity and unexpected handler
 *       failures 500, mirroring rpcv2Cbor. The reserved codes are used for envelope-level failures:
 *       -32700 (unparseable body), -32600 (invalid envelope or content type), -32601 (unknown
 *       method), -32602 (params not deserializable).
 *   <li>{@code error.data} carries the serialized error structure with the fully qualified shape id
 *       in {@code __type} (the rpcv2Cbor convention); clients strip the namespace. {@code
 *       error.message} is the human-readable message ({@code data}'s own {@code message} member
 *       when the caller supplied none).
 *   <li>Event streams (ADR-0023, the authored stream conformance suite beside the unary one): one
 *       WebSocket upgrade GET on the same shared endpoint, every frame a text JSON-RPC envelope —
 *       the opening request envelope selects the operation and carries initial-request members in
 *       {@code params}, events are notifications echoing the opening id inside {@code params}, and
 *       the stream ends with a response envelope for the opening id (result, the error convention
 *       above, or the reserved codes) then the close. Emission lives in {@link
 *       JsonRpc2StreamCodeGen}; the mid-stream grammar lives in the runtime's {@code
 *       eventstream_jsonrpc}.
 * </ul>
 */
final class JsonRpc2Protocol implements ProtocolGenerator {

  static final ShapeId TRAIT = ShapeId.from("smithy.cpp.protocols#jsonRpc2");

  @Override
  public String name() {
    return "jsonRpc2";
  }

  /** The one derivation of the Handle&lt;Op&gt; helper name (definition and call sites). */
  private static String handleFunction(String opName) {
    return "Handle" + opName;
  }

  @Override
  public ShapeId traitId() {
    return TRAIT;
  }

  @Override
  public String contentType() {
    return "application/json";
  }

  @Override
  public boolean usesJsonName() {
    return true;
  }

  @Override
  public List<String> runtimeDeps() {
    return List.of(":json");
  }

  @Override
  public List<String> clientIncludes() {
    return List.of("\"smithy/json/json.h\"", "\"smithy/core/document.h\"");
  }

  @Override
  public void writeClientHelpers(CppWriter w, CppContext context) {
    ProtocolSupport.writeSanitizeErrorCode(w);
    ProtocolSupport.writeParsedErrorStruct(w);
    // jsonRpc2 error identity lives in the envelope's error object rather than
    // a header or the body's top level, so ParseError is protocol-specific:
    // error.code maps onto ParsedError::status (HTTP-like application codes
    // pass through; reserved envelope codes collapse to 400, except -32603
    // which is a retryable 500), error.data becomes the detail document, and
    // data.__type carries the shape identity.
    w.write("// [[maybe_unused]]: only unary response paths parse wire errors; a");
    w.write("// service whose operations all stream never calls this.");
    w.openBlock(
        "[[maybe_unused]] ParsedError ParseError(const smithy::http::HttpResponse& response) {");
    w.write("ParsedError parsed;");
    w.write("parsed.status = response.status;");
    w.write("parsed.message = \"HTTP \" + std::to_string(response.status);");
    w.write("auto doc = smithy::json::Decode(response.body);");
    w.write("if (!doc.ok() || !doc->is_map()) return parsed;");
    w.write("const smithy::Document* error = doc->Find(\"error\");");
    w.write("if (error == nullptr || !error->is_map()) return parsed;");
    w.openBlock(
        "if (const smithy::Document* code = error->Find(\"code\"); "
            + "code != nullptr && code->is_int()) {");
    // Classified on the full int64: narrowing first would alias 2^32+404
    // onto 404 and misroute status/retryability (issue #109).
    w.write("const std::int64_t rpc_code = code->as_int();");
    w.write(
        "parsed.status = rpc_code >= 100 && rpc_code < 600 ? static_cast<int>(rpc_code) "
            + ": (rpc_code == -32603 ? 500 : 400);");
    w.closeBlock("}");
    w.write(
        "if (const smithy::Document* message = error->Find(\"message\"); "
            + "message != nullptr && message->is_string()) parsed.message = message->as_string();");
    w.write(
        "if (const smithy::Document* data = error->Find(\"data\"); "
            + "data != nullptr) parsed.doc = *data;");
    w.openBlock("if (parsed.doc.is_map()) {");
    w.write("const smithy::Document* type = parsed.doc.Find(\"__type\");");
    w.write(
        "if (type != nullptr && type->is_string()) "
            + "parsed.code = helpers::SanitizeErrorCode(type->as_string());");
    w.closeBlock("}");
    w.write("return parsed;");
    w.closeBlock("}");
    w.write("");
    ProtocolSupport.writeGenericError(w);
  }

  @Override
  public void writeOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StructureShape output = ProtocolSupport.outputShape(context, operation);

    String in = ProtocolSupport.writeRpcRequestPrelude(w, context, input);
    w.write("request.target = path_prefix_ + \"/\";");
    w.write("request.headers.Set(\"content-type\", \"application/json\");");
    w.write("smithy::DocumentMap envelope;");
    w.write("envelope.emplace(\"jsonrpc\", smithy::Document(\"2.0\"));");
    w.write("envelope.emplace(\"method\", smithy::Document($S));", operation.getId().getName());
    w.write("envelope.emplace(\"id\", smithy::Document(1));");
    // Operations with no modeled input (smithy.api#Unit) omit params; empty
    // input structures still send an (empty) params object.
    if (!ProtocolSupport.noModeledInput(input)) {
      w.write(
          "envelope.emplace(\"params\", Serialize$L($L));",
          SerdeCodeGen.serdeFunctionSuffix(context, input),
          in);
    }
    w.write("request.body = smithy::json::Encode(smithy::Document(std::move(envelope)));");

    ProtocolSupport.writeRpcSend(w, operation);
    w.write("auto envelope_doc = smithy::json::Decode(response->body);");
    w.write("// Errors are JSON-RPC envelopes on HTTP 200; non-200 means the request");
    w.write("// never reached the protocol layer (router 404, proxy) and parses generically.");
    w.write(
        "const bool is_error = !envelope_doc.ok() || !envelope_doc->is_map() || "
            + "envelope_doc->Find(\"error\") != nullptr;");
    w.write(
        "if (response->status != 200 || is_error) return $L;",
        ProtocolSupport.errorExpression(context, service, operation));

    if (ProtocolSupport.writeEmptyOutputReturn(w, context, output)) {
      return;
    }
    String outType = context.cppSymbols().toSymbol(output).getName();
    w.write("const smithy::Document* result = envelope_doc->Find(\"result\");");
    boolean allOptional = output.members().stream().noneMatch(MemberShape::isRequired);
    if (allOptional) {
      w.write("if (result == nullptr) return $L{};", outType);
    } else {
      w.write(
          "if (result == nullptr) return smithy::Error::Serialization("
              + "\"jsonRpc2: response has no result member\");");
    }
    w.write("return Deserialize$L(*result);", SerdeCodeGen.serdeFunctionSuffix(context, output));
  }

  @Override
  public boolean supportsEventStreams() {
    return true;
  }

  @Override
  public boolean streamsRideJsonRpcEnvelopes() {
    return true;
  }

  @Override
  public String eventPayloadEncode(String docExpr) {
    return "smithy::Blob::FromString(smithy::json::Encode(" + docExpr + "))";
  }

  @Override
  public String eventPayloadDecode(String payloadExpr) {
    return "smithy::json::Decode(" + payloadExpr + ".ToString())";
  }

  @Override
  public void writeStreamingOperationBody(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    JsonRpc2StreamCodeGen.writeStreamingOperationBody(w, context, service, this, operation);
  }

  @Override
  public void writeStreamServerRoutes(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    w.write("// One shared-endpoint stream route (ADR-0023): the wire routes on the");
    w.write("// opening envelope's method, exactly like the unary POST \"/\" above.");
    w.openBlock(
        "(void)stream_router_->Add(\"GET\", \"/\", [handler](const smithy::http::HttpRequest&"
            + " request, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context, smithy::http::WebSocket& socket) {");
    w.write("(void)request;");
    w.write("helpers::ServeJsonRpcStream(*handler, context, socket);");
    w.closeBlock("}, $S);", service.getId().getName());
  }

  @Override
  public void writeStreamSessionRoutes(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    w.openBlock(
        "(void)stream_router_->AddSession(\"GET\", \"/\", [handler](const"
            + " smithy::http::HttpRequest& request, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context, std::shared_ptr<smithy::http::WebSocket> socket) {");
    w.write("(void)request;");
    w.write("(void)context;");
    w.write("helpers::ServeJsonRpcSession(handler, std::move(socket));");
    w.closeBlock("}, $S);", service.getId().getName());
  }

  /** jsonRpc2 error identity travels in error.data.__type; every response echoes the id. */
  private static final ProtocolSupport.ErrorResponseSpec SPEC =
      new ProtocolSupport.ErrorResponseSpec(
          "JsonRpcError", /* errortypeHeader= */ "", ", const smithy::Document& id", ", id");

  /** Set up by writeServerHelpers (always called before the routes are emitted). */
  private ValidationGenerator validation;

  @Override
  public List<String> serverIncludes() {
    return List.of(
        "\"smithy/json/json.h\"", "\"smithy/core/document.h\"", "\"smithy/http/headers.h\"");
  }

  @Override
  public void writeServerHelpers(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    // The one error emitter: a JSON-RPC error envelope on HTTP 200. `code` is
    // the JSON-RPC error code (HTTP-like application codes for modeled/framework
    // errors, reserved -32xxx codes for envelope-level failures); `type` lands
    // in data.__type; an empty `message` falls back to data's message member.
    w.openBlock(
        "smithy::http::HttpResponse $L(int code, const std::string& type, "
            + "const std::string& message, smithy::DocumentMap data, const smithy::Document& id) "
            + "{",
        SPEC.errorFn());
    w.write("if (!type.empty()) data.insert_or_assign(\"__type\", smithy::Document(type));");
    w.write("std::string text = message;");
    w.openBlock("if (text.empty()) {");
    w.write("const auto it = data.find(\"message\");");
    w.write("if (it != data.end() && it->second.is_string()) text = it->second.as_string();");
    w.closeBlock("}");
    w.write("smithy::DocumentMap error;");
    w.write("error.emplace(\"code\", smithy::Document(code));");
    w.write("error.emplace(\"message\", smithy::Document(std::move(text)));");
    w.write("if (!data.empty()) error.emplace(\"data\", smithy::Document(std::move(data)));");
    w.write("smithy::DocumentMap envelope;");
    w.write("envelope.emplace(\"jsonrpc\", smithy::Document(\"2.0\"));");
    w.write("envelope.emplace(\"error\", smithy::Document(std::move(error)));");
    w.write("envelope.emplace(\"id\", id);");
    w.write("smithy::http::HttpResponse response;");
    w.write("response.status = 200;");
    w.write("response.headers.Set(\"content-type\", \"application/json\");");
    w.write("response.body = smithy::json::Encode(smithy::Document(std::move(envelope)));");
    w.write("return response;");
    w.closeBlock("}");
    w.write("");
    ProtocolSupport.writeServerErrorToResponse(w, context, service, operations, SPEC);
    // jsonRpc2 validation identity travels in error.data.__type, as the
    // fully qualified shape id (the rpcv2Cbor convention).
    validation =
        ValidationGenerator.writeWiring(
            w,
            context,
            operations,
            /* alsoEmit= */ false,
            "smithy.framework#ValidationException",
            SPEC);
    for (OperationShape operation : operations) {
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        // Streaming operations never ride the unary POST "/" — their
        // dispatch lives in the stream drivers (ADR-0023).
        continue;
      }
      writeOperationDispatch(w, context, service, operation);
    }
  }

  /**
   * Handle&lt;Op&gt;: params → input → handler → result/error envelope. Templated on the handler
   * class — both server constructors share the unary table (ADR-0021), and both handler classes
   * declare the unary virtuals.
   */
  private void writeOperationDispatch(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    StructureShape output = ProtocolSupport.outputShape(context, operation);
    String inputType = context.cppSymbols().typeRef(input);
    String opName = CppReservedWords.escape(operation.getId().getName());

    w.write("template <typename Handler>");
    w.openBlock(
        "smithy::http::HttpResponse $L(Handler& handler, const smithy::Document& params, "
            + "const smithy::Document& id, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context) {",
        handleFunction(opName));
    w.write("$L input{};", inputType);
    if (ProtocolSupport.noModeledInput(input)) {
      w.write("(void)params;");
    } else {
      w.write(
          "if (!params.is_map()) return helpers::JsonRpcError(-32602, \"SerializationException\", "
              + "\"params must be an object\", {}, id);");
      ProtocolSupport.writeRpcParsedInput(w, context, input, "params", "-32602", SPEC);
    }
    ProtocolSupport.writeRpcDispatch(w, operation, "handler.", SPEC, validation);
    w.write("smithy::DocumentMap envelope;");
    w.write("envelope.emplace(\"jsonrpc\", smithy::Document(\"2.0\"));");
    w.write(
        "envelope.emplace(\"result\", Serialize$L(*outcome));",
        SerdeCodeGen.serdeFunctionSuffix(context, output));
    w.write("envelope.emplace(\"id\", id);");
    w.write("smithy::http::HttpResponse response;");
    w.write("response.status = 200;");
    w.write("response.headers.Set(\"content-type\", \"application/json\");");
    w.write("response.body = smithy::json::Encode(smithy::Document(std::move(envelope)));");
    w.write("return response;");
    w.closeBlock("}");
    w.write("");
  }

  @Override
  public void writeServerRoutes(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    boolean anyCompressed = operations.stream().anyMatch(ProtocolSupport::gzipCompressed);
    // Only the unary dispatch arms consume the context; when every method
    // streams the table is empty and the parameter stays unnamed (Core
    // Guidelines F.9) to keep -Wunused-parameter quiet.
    w.openBlock(
        "(void)router_->Add(\"POST\", \"/\", "
            + "[handler](const smithy::http::HttpRequest& $L, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " $L) -> smithy::http::HttpResponse {",
        anyCompressed ? "raw_request" : "request",
        operations.isEmpty() ? "/*context*/" : "context");
    w.write("smithy::Document id;  // null until the envelope yields one (JSON-RPC 2.0 §5)");
    if (anyCompressed) {
      // The endpoint is shared, so @requestCompression decodes before dispatch
      // (the client compresses the whole envelope); the reserved -32700 code
      // reports an undecodable body, with the envelope id echoed.
      w.write("smithy::http::HttpRequest request = raw_request;");
      ProtocolSupport.writeGzipRequestDecode(
          w, "JsonRpcError", "-32700", "SerializationException", ", id");
    }
    w.write("// A present Content-Type must carry application/json (parameters ignored).");
    w.openBlock(
        "if (const auto content_type = request.headers.Get(\"content-type\"); "
            + "content_type.has_value() && "
            + "smithy::http::MediaTypeOf(*content_type) != \"application/json\") {");
    w.write(
        "return helpers::JsonRpcError(-32600, \"UnsupportedMediaTypeException\", "
            + "\"expected content-type: application/json\", {}, id);");
    w.closeBlock("}");
    w.write("auto decoded = smithy::json::Decode(request.body);");
    w.write("// Envelope failure messages are fixed strings: the conformance suite");
    w.write("// compares bodies exactly, so no decoder detail leaks into the wire.");
    w.write(
        "if (!decoded) return helpers::JsonRpcError(-32700, \"SerializationException\", "
            + "\"request body is not valid JSON\", {}, id);");
    w.write(
        "if (!decoded->is_map()) return helpers::JsonRpcError(-32600, \"SerializationException\", "
            + "\"request is not a JSON-RPC 2.0 call\", {}, id);");
    w.write(
        "if (const smithy::Document* id_doc = decoded->Find(\"id\"); "
            + "id_doc != nullptr) id = *id_doc;");
    w.write("const smithy::Document* version = decoded->Find(\"jsonrpc\");");
    w.openBlock(
        "if (version == nullptr || !version->is_string() || version->as_string() != \"2.0\") {");
    w.write(
        "return helpers::JsonRpcError(-32600, \"SerializationException\", "
            + "\"expected jsonrpc: \\\"2.0\\\"\", {}, id);");
    w.closeBlock("}");
    w.write("const smithy::Document* method = decoded->Find(\"method\");");
    w.openBlock("if (method == nullptr || !method->is_string()) {");
    w.write(
        "return helpers::JsonRpcError(-32600, \"SerializationException\", "
            + "\"expected a string method member\", {}, id);");
    w.closeBlock("}");
    w.write("// Absent/null params deserialize like an empty object.");
    w.write("const smithy::Document empty_params{smithy::DocumentMap{}};");
    w.write("const smithy::Document* params = decoded->Find(\"params\");");
    w.write("if (params == nullptr || params->is_null()) params = &empty_params;");
    w.write("const std::string& method_name = method->as_string();");
    for (OperationShape operation : operations) {
      String wireName = operation.getId().getName();
      String opName = CppReservedWords.escape(wireName);
      w.openBlock("if (method_name == $S) {", wireName);
      w.write(
          "auto response = helpers::$L(*handler, *params, id, context);", handleFunction(opName));
      w.write("response.operation = $S;", wireName);
      w.write("return response;");
      w.closeBlock("}");
    }
    w.write(
        "return helpers::JsonRpcError(-32601, \"UnknownOperationException\", "
            + "\"unknown method: \" + method_name, {}, id);");
    w.closeBlock("});");
  }
}
