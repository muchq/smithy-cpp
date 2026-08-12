package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.MockManifest;

/**
 * Redirects and bodiless statuses (issue #184). A 3xx with a Location header is the shape of a URL
 * shortener, and it reaches the generator by two different routes — the status on the {@code @http}
 * trait, or an {@code @httpResponseCode} member — which used to disagree about whether a redirect
 * was a success. These pin the agreement, and the RFC 9110 rule that some statuses must not carry a
 * body.
 *
 * <p>The end-to-end proof lives in {@code examples/bazel-consumer/redirect_e2e_test.cc}; these are
 * the generator-level assertions that fail fast when the emitted branch changes.
 */
class HttpRedirectCodeGenTest {

  /** Status on the @http trait; the Smithy validator needs suppressing for a non-2xx code. */
  private static final String STATIC_MODEL =
      """
      $version: "2.0"
      namespace test.rest

      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Resolve] }

      @readonly
      @suppress(["HttpResponseCodeSemantics"])
      @http(method: "GET", uri: "/r/{slug}", code: 302)
      operation Resolve {
          input := {
              @required
              @httpLabel
              slug: String
          }
          output := {
              @required
              @httpHeader("Location")
              location: String
          }
      }
      """;

  /** Status chosen at runtime by the handler. */
  private static final String DYNAMIC_MODEL =
      """
      $version: "2.0"
      namespace test.rest

      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Resolve] }

      @readonly
      @http(method: "GET", uri: "/d/{slug}")
      operation Resolve {
          input := {
              @required
              @httpLabel
              slug: String
          }
          output := {
              @required
              @httpResponseCode
              status: Integer

              @required
              @httpHeader("Location")
              location: String
          }
      }
      """;

  /** A modeled 204: the status is known at generation time and forbids a body. */
  private static final String NO_CONTENT_MODEL =
      """
      $version: "2.0"
      namespace test.rest

      use alloy#simpleRestJson

      @simpleRestJson
      service Svc { version: "1", operations: [Erase] }

      @idempotent
      @http(method: "DELETE", uri: "/r/{slug}", code: 204)
      operation Erase {
          input := {
              @required
              @httpLabel
              slug: String
          }
          output := {
              @httpHeader("x-erased")
              erased: String
          }
      }
      """;

  private static MockManifest generate(String model) {
    return PluginTestHarness.generate(model, "test.rest#Svc", "test::rest");
  }

  /**
   * The body of one generated function. The assertions below are about what {@code
   * Build<Op>Response} does and does not emit, and the shared {@code JsonError} helper in the same
   * file sets a content-type and a JSON body of its own — a whole-file {@code contains} would read
   * that helper and pass (or fail) for the wrong reason.
   */
  private static String function(String source, String name) {
    int start = source.indexOf(name + "(const");
    assertTrue(start >= 0, "no function " + name + " in:\n" + source);
    int end = source.indexOf("\n}\n", start);
    assertTrue(end >= 0, "unterminated function " + name + " in:\n" + source);
    return source.substring(start, end);
  }

  @Test
  void staticRedirectRoundTripsStatusAndLocation() {
    MockManifest manifest = generate(STATIC_MODEL);
    String client = manifest.expectFileString("/src/client.cc");
    String server = manifest.expectFileString("/src/server.cc");

    // The client compares against the one modeled status, so a 302 is success.
    assertTrue(client.contains("if (response->status != 302) return"), client);
    assertTrue(client.contains("response->headers.Get(\"Location\")"), client);
    assertTrue(server.contains("response.status = 302;"), server);
    assertTrue(server.contains("response.headers.Set(\"Location\", output.location);"), server);
  }

  @Test
  void dynamicResponseCodeTreats3xxAsSuccess() {
    String client = generate(DYNAMIC_MODEL).expectFileString("/src/client.cc");

    // The regression: the window was `< 200 || > 299`, which rejected every
    // modeled redirect even though the static branch above accepted one.
    assertTrue(
        client.contains("if (response->status < 200 || response->status >= 400) return"), client);
    assertFalse(client.contains("response->status > 299"), client);
    // The status still reaches the caller through the bound member.
    assertTrue(client.contains("out.status = static_cast<"), client);
  }

  @Test
  void dynamicResponseCodeGuardsTheBodyOnStatusesThatForbidOne() {
    String build =
        function(
            generate(DYNAMIC_MODEL).expectFileString("/src/server.cc"), "BuildResolveResponse");

    // RFC 9110: no body on 1xx/204/304. The status is only known at runtime
    // here, so the guard is emitted around the body write rather than decided
    // at generation time.
    assertTrue(
        build.contains(
            "if (response.status >= 200 && response.status != 204 && response.status != 304) {"),
        build);
    assertTrue(build.contains("response.body = smithy::json::Encode("), build);
  }

  @Test
  void modeledNoContentStillSuppressesTheBodyAtGenerationTime() {
    String build =
        function(
            generate(NO_CONTENT_MODEL).expectFileString("/src/server.cc"), "BuildEraseResponse");

    // Known-bodiless status: no runtime guard, no body, no content-type — and
    // the header binding still lands.
    assertTrue(build.contains("response.status = 204;"), build);
    assertTrue(build.contains("response.headers.Set(\"x-erased\""), build);
    assertFalse(build.contains("response.body = smithy::json::Encode("), build);
    assertFalse(
        build.contains("response.headers.Set(\"content-type\", \"application/json\");"), build);
  }

  /**
   * The control for the test above: a 200 on the same shape still sends a body, so a generator that
   * had simply stopped emitting bodies could not pass both.
   */
  @Test
  void a2xxStatusStillSendsABody() {
    String model = NO_CONTENT_MODEL.replace("code: 204", "code: 200");
    String build =
        function(generate(model).expectFileString("/src/server.cc"), "BuildEraseResponse");

    assertTrue(build.contains("response.status = 200;"), build);
    assertTrue(build.contains("response.body = smithy::json::Encode("), build);
    assertTrue(
        build.contains("response.headers.Set(\"content-type\", \"application/json\");"), build);
  }
}
