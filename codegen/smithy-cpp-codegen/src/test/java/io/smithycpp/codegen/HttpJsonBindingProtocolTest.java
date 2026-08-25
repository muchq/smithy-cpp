package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * Pins the wiring of the HTTP+JSON protocol facade: the client half (HttpJsonClientGenerator), the
 * server half (HttpJsonServerGenerator), and the shared binding emitters (HttpBindingCodeGen) must
 * cooperate to produce a complete client.cc/server.cc pair. The byte-level golden check lives in
 * the checked-in fixtures; these assertions guard the seams the split introduced.
 */
class HttpJsonBindingProtocolTest {

  private static final String FILES_MODEL =
      """
      $version: "2.0"
      namespace test.rest

      use alloy#simpleRestJson

      @simpleRestJson
      service Files { version: "1", operations: [PutFile, GetMeta] }

      @http(method: "PUT", uri: "/files/{name}")
      operation PutFile {
          input := {
              @required
              @httpLabel
              name: String

              @httpQuery("tag")
              tag: String

              @httpHeader("x-file-kind")
              kind: String

              @httpPrefixHeaders("x-meta-")
              meta: StringMap

              body: String
          }
          output := {
              @httpResponseCode
              code: Integer

              @httpPrefixHeaders("x-out-")
              echoed: StringMap

              id: String
          }
          errors: [NoSpace]
      }

      @http(method: "GET", uri: "/meta")
      operation GetMeta {
          output := { info: String }
      }

      map StringMap {
          key: String
          value: String
      }

      @error("client")
      @httpError(429)
      structure NoSpace {
          message: String
      }
      """;

  private static MockManifest generateFiles() {
    return PluginTestHarness.generate(FILES_MODEL, "test.rest#Files", "test::rest");
  }

  private static int count(String haystack, String needle) {
    int occurrences = 0;
    for (int at = haystack.indexOf(needle); at >= 0; at = haystack.indexOf(needle, at + 1)) {
      occurrences++;
    }
    return occurrences;
  }

  @Test
  void simpleRestJsonFactoryCarriesTheProtocolIdentity() {
    HttpJsonBindingProtocol protocol = HttpJsonBindingProtocol.simpleRestJson();
    assertEquals("simpleRestJson", protocol.name());
    assertEquals(ShapeId.from("alloy#simpleRestJson"), protocol.traitId());
    assertEquals("application/json", protocol.contentType());
    assertTrue(protocol.usesJsonName());
    // The X-Error-Type header is the discriminator, with status-code fallback.
    assertTrue(protocol.errorStatusFallback());
    assertEquals(List.of(":json"), protocol.runtimeDeps());
  }

  @Test
  void clientEmitsEveryRequestBindingLocation() {
    String client = generateFiles().expectFileString("/src/client.cc");
    assertTrue(client.contains("request.method = \"PUT\";"), client);
    // @httpLabel: URI-encoded path segment.
    assertTrue(client.contains("target += smithy::http::EncodePathSegment("), client);
    // @httpQuery, @httpHeader, @httpPrefixHeaders, and the document body.
    assertTrue(client.contains("query.Add(\"tag\","), client);
    assertTrue(client.contains("request.headers.Set(\"x-file-kind\","), client);
    assertTrue(client.contains("request.headers.Set(\"x-meta-\" + map_key, map_value);"), client);
    assertTrue(client.contains("body_map.emplace(\"body\","), client);
  }

  @Test
  void clientExtractsEveryResponseBindingLocation() {
    String client = generateFiles().expectFileString("/src/client.cc");
    // @httpResponseCode, @httpPrefixHeaders, and the body member.
    assertTrue(client.contains("out.code = static_cast<"), client);
    assertTrue(client.contains("(response->status);"), client);
    assertTrue(
        client.contains("smithy::http::HeaderNameStartsWith(header_name, \"x-out-\")"), client);
    // Error identity: the neutral x-error-type header discriminates modeled errors.
    assertTrue(client.contains("x-error-type"), client);
  }

  @Test
  void serverParsesTheInverseOfTheClientRequest() {
    String server = generateFiles().expectFileString("/src/server.cc");
    assertTrue(server.contains("ParsePutFileInput"), server);
    assertTrue(server.contains("context.labels.at(\"name\")"), server);
    assertTrue(server.contains("if (key == \"tag\")"), server);
    assertTrue(
        server.contains("smithy::http::HeaderNameStartsWith(header_name, \"x-meta-\")"), server);
    assertTrue(server.contains("body_doc->Find(\"body\")"), server);
  }

  @Test
  void floatTextBindingsNarrowThroughTheCheckedHelper() {
    // No checked-in fixture binds a float to a text position, so the golden
    // trees can't pin this: a finite query/label/header value beyond float
    // range must fail the parse via smithy::FloatFromDouble instead of
    // hitting the UB static_cast (issue #109). Doubles stay unnarrowed.
    String server =
        PluginTestHarness.generate(
                """
                $version: "2.0"
                namespace test.rest

                use alloy#simpleRestJson

                @simpleRestJson
                service Nums { version: "1", operations: [GetStats] }

                @http(method: "GET", uri: "/stats")
                operation GetStats {
                    input := {
                        @httpQuery("ratio")
                        ratio: Float

                        @httpQuery("precise")
                        precise: Double
                    }
                    output := { info: String }
                }
                """,
                "test.rest#Nums",
                "test::rest")
            .expectFileString("/src/server.cc");
    assertTrue(server.contains("smithy::FloatFromDouble(*parsed_num)"), server);
    assertTrue(
        server.contains("if (!narrowed_num) return std::move(narrowed_num).error();"), server);
    assertEquals(1, count(server, "FloatFromDouble"), server);
  }

  @Test
  void serverRoutesCarryContentNegotiationAndErrorIdentity() {
    String server = generateFiles().expectFileString("/src/server.cc");
    assertEquals(2, count(server, "router_->Add("), server);
    // 415 then 406, each tagged with the protocol's error-identity header.
    assertTrue(server.contains("JsonError(415, \"\", \"unsupported media type\", {})"), server);
    assertTrue(
        server.contains(
            "error_response.headers.Set(\"x-error-type\", \"UnsupportedMediaTypeException\");"),
        server);
    assertTrue(server.contains("JsonError(406, \"\", \"not acceptable\", {})"), server);
    assertTrue(
        server.contains(
            "error_response.headers.Set(\"x-error-type\", \"NotAcceptableException\");"),
        server);
  }

  @Test
  void serverWritesResponsePrefixHeadersExactlyOnce() {
    // The pre-split serializer emitted the @httpPrefixHeaders loop twice on
    // payload-less responses (harmless — Set is idempotent — but duplicated
    // generated code); pin the single emission.
    String server = generateFiles().expectFileString("/src/server.cc");
    assertTrue(server.contains("BuildPutFileResponse"), server);
    assertEquals(
        1, count(server, "response.headers.Set(\"x-out-\" + map_key, map_value);"), server);
  }

  @Test
  void jsonNameRenamesBodyKeysInBindingAndSerdeAlike() {
    // simpleRestJson honors @jsonName. The binding emitters (client request
    // write, server request read, server response write) and the serde
    // functions must all use the renamed key — a split policy would make the
    // two ends of the wire disagree about the body.
    String model =
        """
        $version: "2.0"
        namespace test.rest
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Put] }
        @http(method: "POST", uri: "/put")
        operation Put {
            input := {
                @jsonName("wire_key")
                renamed: String

                nested: Payload
            }
            output := {
                @jsonName("out_key")
                renamed: String
            }
        }

        structure Payload {
            @jsonName("nested_key")
            inner: String
        }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.rest#Svc", "test::rest");
    String client = manifest.expectFileString("/src/client.cc");
    String server = manifest.expectFileString("/src/server.cc");
    String serde = manifest.expectFileString("/src/serde.cc");
    assertTrue(client.contains("body_map.emplace(\"wire_key\""), client);
    assertTrue(server.contains("Find(\"wire_key\")"), server);
    assertTrue(server.contains("body_map.emplace(\"out_key\""), server);
    // Nested structures go through serde, which applies the same policy.
    assertTrue(serde.contains("\"nested_key\""), serde);
    // Nowhere does the un-renamed member name appear as a JSON key.
    assertTrue(!client.contains("\"renamed\"") && !server.contains("\"renamed\""), client);
  }

  @Test
  void requiredBodyMemberIsStrictOnClientsLenientOnServers() {
    // The one deliberate divergence inside the shared document-body READ
    // (HttpBindingCodeGen.writeDocumentBodyRead): a missing required body
    // member fails the exchange on the client, while the server records a
    // validation failure and keeps parsing so one response carries every
    // failure. Everything else in that emitter is shared by construction.
    String model =
        """
        $version: "2.0"
        namespace test.rest
        use alloy#simpleRestJson

        @simpleRestJson
        service Svc { version: "1", operations: [Put] }
        @http(method: "POST", uri: "/put", code: 200)
        operation Put {
            input := {
                @required
                name: String
            }
            output := {
                @required
                name: String

                @httpResponseCode
                @required
                status: Integer
            }
        }
        """;
    MockManifest manifest = PluginTestHarness.generate(model, "test.rest#Svc", "test::rest");
    String client = manifest.expectFileString("/src/client.cc");
    String server = manifest.expectFileString("/src/server.cc");
    // Client: strict — the required @httpResponseCode member forces the
    // member-by-member body parse, and absence is a Serialization error.
    assertTrue(
        client.contains("smithy::Error::Serialization(\"missing required member: name\")"), client);
    // Server: lenient — absence is recorded and parsing continues.
    assertTrue(server.contains("AddValidationFailure(validation_failures, \"/name\""), server);
  }
}
