package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

/**
 * Direct assertions on ValidationGenerator's emitted constraint checks. The message expectations
 * are NOT derived from this generator's output: they are the exact texts of the official Smithy
 * validation conformance suite (smithy-lang/smithy, smithy-aws-protocol-tests
 * model/restJson1/validation/malformed-*.smithy), so a generator that drifts from the upstream
 * convention fails here even though it agrees with itself. The numeric bounds must additionally use
 * compilable spellings — the int64-min literal was one of issue #43's compile breaks.
 */
class ValidationGeneratorTest {

  private static String generateServer(String modelText) {
    return PluginTestHarness.generate(
            modelText,
            "test.validation#Svc",
            "test::validation",
            b -> b.withMember("mode", "server"))
        .expectFileString("/src/server.cc");
  }

  private static final String CONSTRAINED_MODEL =
      """
      $version: "2.0"
      namespace test.validation
      use smithy.cpp.protocols#jsonRpc2

      @jsonRpc2
      service Svc { version: "1", operations: [Op] }
      operation Op {
          input := {
              @required
              @length(min: 1, max: 8)
              @pattern("^[a-z]+$")
              name: String

              @range(min: -9223372036854775808, max: 9223372036854775807)
              extremes: Long

              @range(min: 1, max: 100)
              count: Integer

              @length(min: 1)
              tags: TagList
          }
      }

      list TagList { member: String }
      """;

  @Test
  void lengthMessagesAreSuiteExact() {
    String server = generateServer(CONSTRAINED_MODEL);
    assertTrue(
        server.contains(
            "failed to satisfy constraint: Member must have length between 1 and 8, inclusive"),
        server);
  }

  @Test
  void rangeMessagesAreSuiteExactAndBoundsCompile() {
    String server = generateServer(CONSTRAINED_MODEL);
    assertTrue(
        server.contains(
            "failed to satisfy constraint: Member must be between 1 and 100, inclusive"),
        server);
    // The int64-min bound must be the header-free idiom, never the
    // ill-formed decimal literal (issue #43) — while the human-readable
    // message keeps the decimal spelling.
    assertTrue(server.contains("(-9223372036854775807LL - 1)"), server);
    assertTrue(
        server.contains(
            "Member must be between -9223372036854775808 and 9223372036854775807, inclusive"),
        server);
  }

  @Test
  void patternsCompileOnceIntoTheLinearTimeEngine() {
    String server = generateServer(CONSTRAINED_MODEL);
    assertTrue(server.contains("smithy::Regex::Compile(R\"__smithy(^[a-z]+$)__smithy\")"), server);
    // The message splices the pattern in as a separate escaped literal (the
    // issue-#43 injection fix), so the text is asserted in its two pieces.
    assertTrue(
        server.contains(
            "Member must satisfy regular expression pattern: \" + std::string(\"^[a-z]+$\")"),
        server);
    // static const: compiled once per process, not per request.
    assertTrue(server.contains("static const smithy::Outcome<smithy::Regex>"), server);
  }

  @Test
  void lengthCountsCodePointsForStringsAndElementsForCollections() {
    String server = generateServer(CONSTRAINED_MODEL);
    // Strings measure UTF-8 code points, not bytes; collections measure
    // elements. A min-only @length reports the one-sided message — the
    // upstream suite's exact form ("Member must have length greater than or
    // equal to N", malformed-length.smithy), not a synthesized
    // between-1-and-int64max range.
    assertTrue(server.contains("smithy::Utf8CodePointCount(value.name)"), server);
    assertTrue(server.contains("(*value.tags).size()"), server);
    assertTrue(server.contains("Member must have length greater than or equal to 1"), server);
  }

  @Test
  void intEnumMembershipFollowsTheStringEnumPolicy() {
    // intEnum members validate against the value set the way string enums do
    // (issue #109; upstream suite message form, malformed-enum.smithy
    // convention with the int spellings smithy-rs/-typescript emit): validity
    // spans every member — @internal included — while the advertised set in
    // the message omits internal members.
    String server =
        generateServer(
            """
            $version: "2.0"
            namespace test.validation
            use smithy.cpp.protocols#jsonRpc2

            @jsonRpc2
            service Svc { version: "1", operations: [Op] }
            operation Op { input := { weight: Weight } }

            intEnum Weight {
                LIGHT = 1
                HEAVY = 2

                @internal
                LEGACY = 99
            }
            """);
    assertTrue(server.contains("!= Weight::kLight"), server);
    assertTrue(server.contains("!= Weight::kHeavy"), server);
    assertTrue(server.contains("!= Weight::kLegacy"), server);
    assertTrue(
        server.contains("failed to satisfy constraint: Member must satisfy enum value set: [1, 2]"),
        server);
  }
}
