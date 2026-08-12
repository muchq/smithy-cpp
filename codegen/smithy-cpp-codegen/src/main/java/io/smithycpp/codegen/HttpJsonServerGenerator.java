package io.smithycpp.codegen;

import java.util.List;
import java.util.Map;
import software.amazon.smithy.model.knowledge.HttpBinding;
import software.amazon.smithy.model.knowledge.HttpBindingIndex;
import software.amazon.smithy.model.pattern.SmithyPattern;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.traits.HttpTrait;

/**
 * The server half of the HTTP+JSON protocol: the JsonError/ErrorToResponse helpers, validation
 * wiring, one Parse&lt;Op&gt;Input / Build&lt;Op&gt;Response function pair per operation (each the
 * wire inverse of the client's request/response handling), and the per-operation route with its
 * content negotiation (415/406). Shared read/write emission lives in {@link HttpBindingCodeGen}.
 *
 * <p>Stateful: {@link #writeHelpers} builds the validation plan the routes consult, so it always
 * runs before {@link #writeRoute} (ServerGenerator emits helpers first).
 */
final class HttpJsonServerGenerator {

  /**
   * The response header carrying modeled-error identity (the error shape name), or "" when the
   * protocol carries no error header. simpleRestJson uses {@code x-error-type}.
   */
  private final String errorTypeHeaderName;

  /** Mirror of the owning protocol's usesJsonName() — body keys must match the serde functions. */
  private final boolean useJsonName;

  /** Set up by writeHelpers (always called before the routes are emitted). */
  private ValidationGenerator validation;

  /** Whether the service emits ValidationErrorResponse (constraints or top-level @required). */
  private boolean emitsValidation;

  HttpJsonServerGenerator(String errorTypeHeaderName, boolean useJsonName) {
    this.errorTypeHeaderName = errorTypeHeaderName;
    this.useJsonName = useJsonName;
  }

  /** The one derivation of the Parse&lt;Op&gt;Input helper name (definition and call sites). */
  private static String parseInputFunction(String opName) {
    return "Parse" + opName + "Input";
  }

  /** The one derivation of the Build&lt;Op&gt;Response helper name (definition and call sites). */
  private static String buildResponseFunction(String opName) {
    return "Build" + opName + "Response";
  }

  List<String> includes() {
    return List.of(
        "\"smithy/json/json.h\"",
        "\"smithy/core/base64.h\"",
        "\"smithy/core/document_serde.h\"",
        "\"smithy/core/blob.h\"",
        "\"smithy/http/headers.h\"",
        "<cstdint>",
        "<cstdlib>",
        "<limits>");
  }

  /** Emits the error-type header set; a no-op when the protocol has no error header. */
  private void writeErrorTypeHeader(CppWriter w, String variable, String errorType) {
    if (!errorTypeHeaderName.isEmpty()) {
      w.write("$L.headers.Set($S, $S);", variable, errorTypeHeaderName, errorType);
    }
  }

  void writeHelpers(
      CppWriter w, CppContext context, ServiceShape service, List<OperationShape> operations) {
    SerdeCodeGen serde = new SerdeCodeGen(context, useJsonName);
    ProtocolSupport.ErrorResponseSpec spec =
        new ProtocolSupport.ErrorResponseSpec("JsonError", errorTypeHeaderName);
    ProtocolSupport.writeNumericParseHelpers(w);
    ProtocolSupport.writeErrorBodyHelper(
        w,
        spec.errorFn(),
        "application/json",
        "smithy::json::Encode(smithy::Document(std::move(body)))");
    ProtocolSupport.writeServerErrorToResponse(w, context, service, operations, spec);
    validation =
        ValidationGenerator.writeWiring(
            w,
            context,
            operations,
            /* alsoEmit= */ anyTopLevelRequired(context, operations),
            /* validationErrorCode= */ "",
            spec);
    emitsValidation = validation.wiringEmitted();
    for (OperationShape operation : operations) {
      writeParseInputFunction(w, context, serde, operation);
      if (EventStreamCodeGen.streaming(context.model(), operation)) {
        // Streaming operations answer over the session, never an HTTP
        // response; only their upgrade-request parse is shared (ADR-0016).
        continue;
      }
      writeBuildResponseFunction(w, context, serde, operation);
    }
  }

  /**
   * Whether any operation has a required top-level body/query/header member: those absences are
   * ValidationException failures ("Member must not be null"), not serialization errors. Absent
   * labels never route here, and nested required members stay serde-strict.
   */
  private static boolean anyTopLevelRequired(CppContext context, List<OperationShape> operations) {
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    for (OperationShape operation : operations) {
      MemberShape streamMember = EventStreamCodeGen.inputStreamMember(context.model(), operation);
      for (HttpBinding binding : index.getRequestBindings(operation).values()) {
        if (binding.getMember().equals(streamMember)) {
          // The event-stream union is the session, not a parsed member.
          continue;
        }
        boolean location =
            switch (binding.getLocation()) {
              case DOCUMENT, QUERY, HEADER -> true;
              default -> false;
            };
        if (location && binding.getMember().isRequired()) {
          return true;
        }
      }
    }
    return false;
  }

  /** Emits Parse<Op>Input(request, context) -> Outcome<Input> (inverse of the client request). */
  private void writeParseInputFunction(
      CppWriter w, CppContext context, SerdeCodeGen serde, OperationShape operation) {
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    StructureShape input = ProtocolSupport.inputShape(context, operation);
    String inputType = context.cppSymbols().typeRef(input);
    String opName = CppReservedWords.escape(operation.getId().getName());
    HttpBindingCodeGen.RequestBindings req =
        HttpBindingCodeGen.RequestBindings.of(
            index, operation, EventStreamCodeGen.inputStreamMember(context.model(), operation));
    Map<String, HttpBinding> labels = req.labels();
    Map<String, HttpBinding> queries = req.queries();
    Map<String, HttpBinding> headers = req.headers();
    List<HttpBinding> body = req.body();
    HttpBinding queryParams = req.queryParams();
    HttpBinding payload = req.payload();
    HttpBinding prefixHeaders = req.prefixHeaders();

    w.openBlock(
        "smithy::Outcome<$L> $L(const smithy::http::HttpRequest& request, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context, "
            + "std::vector<smithy::server::ValidationFailure>* validation_failures) {",
        inputType,
        parseInputFunction(opName));
    w.write("(void)request;");
    w.write("(void)context;");
    w.write("(void)validation_failures;");
    w.write("$L input{};", inputType);
    for (HttpBinding binding : labels.values()) {
      // @httpLabel members are always required, and route matching guarantees
      // the label was captured.
      MemberShape member = binding.getMember();
      w.openBlock("{");
      w.write("const std::string& label_value = context.labels.at($S);", binding.getLocationName());
      HttpBindingCodeGen.writeTextValueInto(
          w,
          context,
          serde,
          member,
          "label_value",
          "input." + context.cppSymbols().toMemberName(member),
          "smithy::TimestampFormat::kDateTime",
          /* push= */ false);
      w.closeBlock("}");
    }
    for (HttpBinding binding : headers.values()) {
      HttpBindingCodeGen.writeHeaderReadBinding(
          w, context, serde, binding, "request.headers", "input.");
      if (binding.getMember().isRequired()) {
        writeRequiredAbsenceFailure(
            w,
            binding.getMember(),
            "!request.headers.Get(\"" + binding.getLocationName() + "\").has_value()");
      }
    }
    if (!queries.isEmpty() || queryParams != null) {
      for (HttpBinding binding : queries.values()) {
        if (binding.getMember().isRequired()) {
          w.write("bool saw_$L = false;", context.cppSymbols().toMemberName(binding.getMember()));
        }
      }
      w.openBlock("for (const auto& [key, value] : context.query_params) {");
      for (HttpBinding binding : queries.values()) {
        MemberShape member = binding.getMember();
        Shape target = context.model().expectShape(member.getTarget());
        String field = "input." + context.cppSymbols().toMemberName(member);
        w.openBlock("if (key == $S) {", binding.getLocationName());
        if (member.isRequired()) {
          w.write("saw_$L = true;", context.cppSymbols().toMemberName(member));
        }
        if (target.isListShape()) {
          MemberShape element = target.asListShape().orElseThrow().getMember();
          boolean plainQuery = MemberDefaults.plain(context.model(), member);
          if (!plainQuery) {
            w.write("if (!$L.has_value()) $L.emplace();", field, field);
          }
          String container = plainQuery ? field : "(*" + field + ")";
          HttpBindingCodeGen.writeTextValueInto(
              w,
              context,
              serde,
              element,
              "value",
              container + ".push_back",
              "smithy::TimestampFormat::kDateTime",
              /* push= */ true);
        } else {
          HttpBindingCodeGen.writeTextValueInto(
              w,
              context,
              serde,
              member,
              "value",
              field,
              "smithy::TimestampFormat::kDateTime",
              /* push= */ false);
        }
        w.write("continue;");
        w.closeBlock("}");
      }
      w.closeBlock("}");
      for (HttpBinding binding : queries.values()) {
        if (binding.getMember().isRequired()) {
          writeRequiredAbsenceFailure(
              w,
              binding.getMember(),
              "!saw_" + context.cppSymbols().toMemberName(binding.getMember()));
        }
      }
      if (queryParams != null) {
        MemberShape member = queryParams.getMember();
        var map = context.model().expectShape(member.getTarget()).asMapShape().orElseThrow();
        Shape valueTarget = context.model().expectShape(map.getValue().getTarget());
        String field = "input." + context.cppSymbols().toMemberName(member);
        w.write("// Servers put every query parameter in the @httpQueryParams map,");
        w.write("// including ones bound to other members.");
        w.openBlock("for (const auto& [key, value] : context.query_params) {");
        boolean plainMap = MemberDefaults.plain(context.model(), member);
        if (!plainMap) {
          w.write("if (!$L.has_value()) $L.emplace();", field, field);
        }
        String container = plainMap ? field : "(*" + field + ")";
        if (valueTarget.isListShape()) {
          w.write("$L[key].push_back(value);", container);
        } else {
          w.write("$L.emplace(key, value);", container);
        }
        w.closeBlock("}");
      }
    }
    if (prefixHeaders != null) {
      HttpBindingCodeGen.writePrefixHeadersRead(
          w, context, prefixHeaders, "request.headers", "input.");
    }
    if (payload != null) {
      HttpBindingCodeGen.writePayloadRead(w, context, serde, payload, "request.body", "input.");
    }
    if (!body.isEmpty()) {
      HttpBindingCodeGen.writeDocumentBodyRead(
          w,
          serde,
          body,
          "request.body",
          "input.",
          // The bare type name: this lands in wire error-message paths
          // ("GetCityInput.name"), not in code.
          context.cppSymbols().toSymbol(input).getName(),
          opName,
          (w2, member, deserializeMember) -> {
            // Servers record the absence and keep parsing, so one response
            // carries every validation failure.
            w2.openBlock("if ($L) {", SerdeCodeGen.MEMBER_ABSENT);
            w2.write(
                "helpers::AddValidationFailure(validation_failures, $S, $S);",
                "/" + member.getMemberName(),
                ValidationGenerator.memberMustNotBeNull("/" + member.getMemberName()));
            w2.closeBlock("} else {");
            w2.indent();
            deserializeMember.run();
            w2.closeBlock("}");
          });
    }
    // @default on @input members: clients skip them when unset, servers fill
    // the default for whatever the wire left unset (any binding location).
    for (MemberShape member : input.members()) {
      if (!MemberDefaults.fillOnParse(context.model(), member)) {
        continue;
      }
      String field = "input." + context.cppSymbols().toMemberName(member);
      w.write(
          "if (!$L.has_value()) $L = $L;", field, field, MemberDefaults.literal(context, member));
    }
    w.write("return input;");
    w.closeBlock("}");
    w.write("");
  }

  /** Records a "Member must not be null" failure when {@code absentCondition} holds. */
  private void writeRequiredAbsenceFailure(
      CppWriter w, MemberShape member, String absentCondition) {
    String path = "/" + member.getMemberName();
    w.write(
        "if ($L) AddValidationFailure(validation_failures, $S, $S);",
        absentCondition,
        path,
        ValidationGenerator.memberMustNotBeNull(path));
  }

  /**
   * Emits Build<Op>Response(output) (inverse of the client response handling). Build, not
   * Serialize: the serde functions own the Serialize/Deserialize<Shape> namespace, and a same-named
   * file-local helper would hide them for shapes named <Op>Response.
   */
  private void writeBuildResponseFunction(
      CppWriter w, CppContext context, SerdeCodeGen serde, OperationShape operation) {
    HttpBindingIndex index = HttpBindingIndex.of(context.model());
    HttpTrait http = operation.expectTrait(HttpTrait.class);
    StructureShape output = ProtocolSupport.outputShape(context, operation);
    String outputType = context.cppSymbols().typeRef(output);
    String opName = CppReservedWords.escape(operation.getId().getName());
    HttpBindingCodeGen.ResponseBindings resp =
        HttpBindingCodeGen.ResponseBindings.of(index, operation);
    Map<String, HttpBinding> responseHeaders = resp.headers();
    List<HttpBinding> responseBody = resp.body();
    HttpBinding responseCode = resp.responseCode();
    HttpBinding responsePayload = resp.payload();
    HttpBinding responsePrefixHeaders = resp.prefixHeaders();

    w.openBlock(
        "smithy::http::HttpResponse $L(const $L& output) {",
        buildResponseFunction(opName),
        outputType);
    w.write("(void)output;");
    w.write("smithy::http::HttpResponse response;");
    w.write("response.status = $L;", http.getCode());
    if (responseCode != null) {
      MemberShape codeMember = responseCode.getMember();
      String field = "output." + context.cppSymbols().toMemberName(codeMember);
      if (MemberDefaults.plain(context.model(), codeMember)) {
        w.write("response.status = static_cast<int>($L);", field);
      } else {
        w.write("if ($L.has_value()) response.status = static_cast<int>(*$L);", field, field);
      }
    }
    for (HttpBinding binding : responseHeaders.values()) {
      HttpBindingCodeGen.writeHeaderWriteBinding(
          w, context, serde, binding, "output", "response.headers");
    }
    if (responsePrefixHeaders != null) {
      HttpBindingCodeGen.writePrefixHeadersWrite(
          w, context, responsePrefixHeaders, "output", "response");
    }
    if (responsePayload != null) {
      HttpBindingCodeGen.writePayloadWrite(
          w, context, serde, operation, responsePayload, "output", "response", false);
      w.write("return response;");
      w.closeBlock("}");
      w.write("");
      return;
    }
    // RFC 9110 forbids a body on 1xx, 204 and 304 responses; a JSON "{}" on
    // one of those is a protocol violation, not just waste (a 304 carrying a
    // body misleads every cache between here and the client).
    if (responseCode == null && statusMustNotHaveBody(http.getCode())) {
      w.write("return response;");
      w.closeBlock("}");
      w.write("");
      return;
    }
    // With @httpResponseCode the status is only known at runtime, so the guard
    // has to be too. Everything else keeps the restJson1 rule: servers always
    // produce a JSON body (at minimum "{}").
    if (responseCode != null) {
      w.openBlock(
          "if (response.status >= 200 && response.status != 204 && response.status != 304) {");
    }
    w.write("smithy::DocumentMap body_map;");
    HttpBindingCodeGen.writeDocumentBodyMap(w, serde, responseBody, "output");
    w.write("response.headers.Set(\"content-type\", \"application/json\");");
    w.write("response.body = smithy::json::Encode(smithy::Document(std::move(body_map)));");
    if (responseCode != null) {
      w.closeBlock("}");
    }
    w.write("return response;");
    w.closeBlock("}");
    w.write("");
  }

  /**
   * RFC 9110 statuses that must never carry a response body: the 1xx informational range, 204 No
   * Content, and 304 Not Modified. 3xx is deliberately absent — a redirect *may* carry a body, and
   * alloy's own CustomCodeOutput conformance case pins "{}" at status 399.
   */
  private static boolean statusMustNotHaveBody(int status) {
    return (status >= 100 && status <= 199) || status == 204 || status == 304;
  }

  /** The runtime Router/WebSocketRouter pattern for an @http URI (shared grammar). */
  private static String routePattern(HttpTrait http) {
    StringBuilder pattern = new StringBuilder();
    for (SmithyPattern.Segment segment : http.getUri().getSegments()) {
      pattern.append('/');
      if (segment.isGreedyLabel()) {
        pattern.append('{').append(segment.getContent()).append("+}");
      } else if (segment.isLabel()) {
        pattern.append('{').append(segment.getContent()).append('}');
      } else {
        pattern.append(segment.getContent());
      }
    }
    if (pattern.length() == 0) {
      pattern.append('/');
    }
    return pattern.toString();
  }

  void writeRoute(CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    HttpTrait http = operation.expectTrait(HttpTrait.class);
    String opName = CppReservedWords.escape(operation.getId().getName());
    String pattern = routePattern(http);
    HttpBindingIndex bindingIndex = HttpBindingIndex.of(context.model());
    HttpBindingCodeGen.RequestBindings req =
        HttpBindingCodeGen.RequestBindings.of(bindingIndex, operation);
    HttpBindingCodeGen.ResponseBindings resp =
        HttpBindingCodeGen.ResponseBindings.of(bindingIndex, operation);
    boolean hasResponseContent = !resp.body().isEmpty() || resp.payload() != null;
    boolean compressed = ProtocolSupport.gzipCompressed(operation);
    w.openBlock(
        "(void)router_->Add($S, $S, [handler](const smithy::http::HttpRequest& $L, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context) -> smithy::http::HttpResponse {",
        http.getMethod(),
        pattern,
        compressed ? "raw_request" : "request");
    if (compressed) {
      w.write("smithy::http::HttpRequest request = raw_request;");
      ProtocolSupport.writeRequestDecompression(w, operation, "JsonError", "");
    }
    boolean noModeledInput =
        ProtocolSupport.noModeledInput(ProtocolSupport.inputShape(context, operation));
    w.write("// Content-Type validation per the HTTP binding spec (415), then Accept (406);");
    w.write("// the malformed-request suite pins the error-identity headers. A missing");
    w.write("// content-type is tolerated, and blob payloads without @mediaType accept");
    w.write("// any content type / accept.");
    if (noModeledInput) {
      w.openBlock("if (request.headers.Get(\"content-type\").has_value()) {");
      w.write(
          "auto error_response = helpers::JsonError(415, \"\", \"unsupported media type\", {});");
      writeErrorTypeHeader(w, "error_response", "UnsupportedMediaTypeException");
      w.write("return error_response;");
      w.closeBlock("}");
    } else if (!HttpBindingCodeGen.lenientPayload(context, operation, /* response= */ false)) {
      String requestContentType =
          HttpBindingCodeGen.payloadContentType(context, operation, /* isRequest= */ true);
      String expected = requestContentType.isEmpty() ? "application/json" : requestContentType;
      boolean hasRequestPayload = req.payload() != null;
      // Payload-bound requests additionally require a content-type once a body
      // is present; document bodies tolerate a missing header (the server-mode
      // httpRequestTests pin that leniency).
      String condition =
          hasRequestPayload
              ? "content_type.has_value() ? smithy::http::MediaTypeOf(*content_type) != $S "
                  + ": !request.body.empty()"
              : "content_type.has_value() && smithy::http::MediaTypeOf(*content_type) != $S";
      w.openBlock(
          "if (const auto content_type = request.headers.Get(\"content-type\"); "
              + condition
              + ") {",
          expected);
      w.write(
          "auto error_response = helpers::JsonError(415, \"\", \"unsupported media type\", {});");
      writeErrorTypeHeader(w, "error_response", "UnsupportedMediaTypeException");
      w.write("return error_response;");
      w.closeBlock("}");
    }
    if (hasResponseContent
        && !HttpBindingCodeGen.lenientPayload(context, operation, /* response= */ true)) {
      String determined =
          HttpBindingCodeGen.payloadContentType(context, operation, /* isRequest= */ false);
      String responseContentType = determined.isEmpty() ? "application/json" : determined;
      w.openBlock(
          "if (const auto accept = request.headers.Get(\"accept\"); accept.has_value() && "
              + "!smithy::http::AcceptMatches(*accept, $S)) {",
          responseContentType);
      w.write("auto error_response = helpers::JsonError(406, \"\", \"not acceptable\", {});");
      writeErrorTypeHeader(w, "error_response", "NotAcceptableException");
      w.write("return error_response;");
      w.closeBlock("}");
    }
    w.write("std::vector<smithy::server::ValidationFailure> validation_failures;");
    w.write(
        "auto input = helpers::$L(request, context, &validation_failures);",
        parseInputFunction(opName));
    if (emitsValidation) {
      w.write(
          "if (!validation_failures.empty()) "
              + "return helpers::ValidationErrorResponse(validation_failures);");
    }
    w.write("if (!input) return helpers::ErrorToResponse(input.error());");
    if (validation.validates(operation)) {
      w.write(
          "helpers::$L(*input, \"\", &validation_failures);",
          validation.validatorNameFor(operation));
      w.write(
          "if (!validation_failures.empty()) "
              + "return helpers::ValidationErrorResponse(validation_failures);");
    }
    w.write("auto outcome = handler->$L(*input, context);", opName);
    w.write("if (!outcome) return helpers::ErrorToResponse(outcome.error());");
    w.write("return helpers::$L(*outcome);", buildResponseFunction(opName));
    w.closeBlock("}, $S);", operation.getId().getName());
  }

  /**
   * One streaming operation's WebSocket route (ADR-0016): the upgrade request parses through the
   * same Parse&lt;Op&gt;Input as a unary request (labels, query, headers; the accepted upgrade has
   * no body), refusals answer with one exception message before the close, and the handler borrows
   * the typed session for the callback's lifetime.
   */
  void writeStreamRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    HttpTrait http = operation.expectTrait(HttpTrait.class);
    String opName = CppReservedWords.escape(operation.getId().getName());
    // Always GET: a WebSocket upgrade is a GET on the wire (ADR-0016)
    // whatever method the operation models, and the route must match what
    // arrives.
    w.openBlock(
        "(void)stream_router_->Add(\"GET\", $S, [handler](const smithy::http::HttpRequest&"
            + " request, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context, smithy::http::WebSocket& socket) {",
        routePattern(http));
    w.write("std::vector<smithy::server::ValidationFailure> validation_failures;");
    w.write(
        "auto input = helpers::$L(request, context, &validation_failures);",
        parseInputFunction(opName));
    w.openBlock("if (!input) {");
    w.write("(void)socket.Send(helpers::Build$LExceptionMessage(input.error()));", opName);
    w.write("socket.Close();");
    w.write("return;");
    w.closeBlock("}");
    // The refusal checks mirror unary writeRoute's guards exactly: the
    // parse-recorded check only when the service emits validation at all,
    // the validator call + check only when this operation's input is
    // constrained — a service that validates nothing gets no dead check.
    if (emitsValidation) {
      writeStreamValidationRefusal(w, opName, "socket.");
    }
    if (validation.validates(operation)) {
      w.write(
          "helpers::$L(*input, \"\", &validation_failures);",
          validation.validatorNameFor(operation));
      writeStreamValidationRefusal(w, opName, "socket.");
    }
    EventStreamCodeGen.writeServeAndClose(w, context, operation, "*input");
    w.closeBlock("}, $S);", operation.getId().getName());
  }

  /**
   * One streaming operation's shared-session route (ADR-0021): the same parse and refusals as
   * {@link #writeStreamRoute} — before any coroutine exists, on the launch callback — then the
   * owned socket and parsed input go to the generated async wrapper; the callback returns at the
   * handler's first suspension, never waiting for the session to end (the launch-point contract).
   */
  void writeStreamSessionRoute(
      CppWriter w, CppContext context, ServiceShape service, OperationShape operation) {
    HttpTrait http = operation.expectTrait(HttpTrait.class);
    String opName = CppReservedWords.escape(operation.getId().getName());
    w.openBlock(
        "(void)stream_router_->AddSession(\"GET\", $S, [handler](const smithy::http::HttpRequest&"
            + " request, "
            + ProtocolSupport.REQUEST_CONTEXT_PARAM
            + " context, std::shared_ptr<smithy::http::WebSocket> socket) {",
        routePattern(http));
    w.write("std::vector<smithy::server::ValidationFailure> validation_failures;");
    w.write(
        "auto input = helpers::$L(request, context, &validation_failures);",
        parseInputFunction(opName));
    w.openBlock("if (!input) {");
    w.write("(void)socket->Send(helpers::Build$LExceptionMessage(input.error()));", opName);
    w.write("socket->Close();");
    w.write("return;");
    w.closeBlock("}");
    if (emitsValidation) {
      writeStreamValidationRefusal(w, opName, "socket->");
    }
    if (validation.validates(operation)) {
      w.write(
          "helpers::$L(*input, \"\", &validation_failures);",
          validation.validatorNameFor(operation));
      writeStreamValidationRefusal(w, opName, "socket->");
    }
    EventStreamCodeGen.writeLaunchAsync(w, operation, "*std::move(input)");
    w.closeBlock("}, $S);", operation.getId().getName());
  }

  /**
   * The streaming analog of ValidationErrorResponse: a refused upgrade answers over the session —
   * one SerializationException exception message carrying the first failure, then the close. One
   * helper serves both seams; {@code socketAccess} is "socket." (borrowed reference) or "socket->"
   * (the shared seam's owned pointer), so the refusal semantics cannot drift between them.
   */
  private static void writeStreamValidationRefusal(
      CppWriter w, String opName, String socketAccess) {
    w.openBlock("if (!validation_failures.empty()) {");
    w.write(
        "(void)$LSend(helpers::Build$LExceptionMessage("
            + "smithy::Error::Validation(validation_failures.front().message)));",
        socketAccess,
        opName);
    w.write("$LClose();", socketAccess);
    w.write("return;");
    w.closeBlock("}");
  }
}
