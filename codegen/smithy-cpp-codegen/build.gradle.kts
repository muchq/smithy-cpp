description = "Core Smithy to C++ code generator (smithy-build plugin)"

val repoRoot: File = rootProject.projectDir.parentFile

fun registerFixtureTask(
    taskName: String,
    modelPath: String,
    service: String,
    cppNamespace: String,
    outputPath: String,
) = tasks.register<JavaExec>(taskName) {
    group = "smithy-cpp"
    description = "Regenerates $outputPath from $modelPath"
    classpath = sourceSets["main"].runtimeClasspath
    mainClass = "io.smithycpp.codegen.CppCodegenRunner"
    args = listOf(
        "--model", File(repoRoot, modelPath).absolutePath,
        "--service", service,
        "--namespace", cppNamespace,
        "--runtime-target", "//runtime:core",
        "--output", File(repoRoot, outputPath).absolutePath,
        "--tests-package", "//" + outputPath,
        "--integration-tests", "true",
    )
    doFirst {
        project.delete(File(repoRoot, outputPath))
    }
}

val generateWeatherFixture = registerFixtureTask(
    "generateWeatherFixture",
    "examples/weather/model/weather.smithy",
    "example.weather#Weather",
    "example::weather",
    "examples/weather/generated",
)

val generateCafeFixture = registerFixtureTask(
    "generateCafeFixture",
    "examples/cafe/model/cafe.smithy",
    "example.cafe#Cafe",
    "example::cafe",
    "examples/cafe/generated",
)

// Full-duplex chat fixture (ADR-0016): bidirectional + server-push event
// streams next to a unary neighbor, the Phase 8 exit-criterion example.
val generateChatFixture = registerFixtureTask(
    "generateChatFixture",
    "examples/chat/model/chat.smithy",
    "example.chat#Chat",
    "example::chat",
    "examples/chat/generated",
)

// simpleRestJson (alloy) fixture: the vendor-neutral REST/JSON protocol.
val generateBookstoreFixture = registerFixtureTask(
    "generateBookstoreFixture",
    "examples/simplerestjson/model/bookstore.smithy",
    "example.bookstore#Bookstore",
    "example::bookstore",
    "examples/simplerestjson/generated",
)

// The kitchen-sink round-trip fixture: one model, two protocol variants, so the
// Phase 5 integration matrix covers REST and RPC with the same shapes.
val generateRoundTripRestFixture = registerFixtureTask(
    "generateRoundTripRestFixture",
    "examples/roundtrip/model/roundtrip.smithy",
    "example.roundtrip#RoundTripRest",
    "example::roundtrip::rest",
    "examples/roundtrip/rest/generated",
)

val generateRoundTripRpcFixture = registerFixtureTask(
    "generateRoundTripRpcFixture",
    "examples/roundtrip/model/roundtrip.smithy",
    "example.roundtrip#RoundTripRpc",
    "example::roundtrip::rpc",
    "examples/roundtrip/rpc/generated",
)

// jsonRpc2 showcase example: a classic single-endpoint JSON-RPC calculator.
val generateCalculatorFixture = registerFixtureTask(
    "generateCalculatorFixture",
    "examples/jsonrpc2/model/calculator.smithy",
    "example.calculator#Calculator",
    "example::calculator",
    "examples/jsonrpc2/generated",
)

val generateRoundTripJsonRpcFixture = registerFixtureTask(
    "generateRoundTripJsonRpcFixture",
    "examples/roundtrip/model/roundtrip.smithy",
    "example.roundtrip#RoundTripJsonRpc",
    "example::roundtrip::jsonrpc",
    "examples/roundtrip/jsonrpc/generated",
)

// The official protocol-test suite models, kept off the main runtime classpath
// so ordinary fixture generation doesn't assemble them.
val protocolTestModels: Configuration by configurations.creating

fun registerProtocolTestTask(
    taskName: String,
    service: String,
    cppNamespace: String,
    outputPath: String,
    omitOperations: List<String> = emptyList(),
    malformedTests: Boolean = false,
    // Authored suites (jsonRpc2 has no published jar) load from a local model
    // file instead of the protocolTestModels classpath.
    modelPath: String? = null,
) = tasks.register<JavaExec>(taskName) {
    group = "smithy-cpp"
    description = "Regenerates $outputPath from the protocol test suite for $service"
    classpath = sourceSets["main"].runtimeClasspath + protocolTestModels
    mainClass = "io.smithycpp.codegen.CppCodegenRunner"
    args = listOf(
        "--service", service,
        "--namespace", cppNamespace,
        "--runtime-target", "//runtime:core",
        "--output", File(repoRoot, outputPath).absolutePath,
        "--tests-package", "//" + outputPath,
    ) + (modelPath?.let { listOf("--model", File(repoRoot, it).absolutePath) } ?: emptyList()) +
        omitOperations.flatMap { listOf("--omit-operation", it) } +
        (if (malformedTests) listOf("--malformed-tests", "true") else emptyList())
    doFirst {
        project.delete(File(repoRoot, outputPath))
    }
}

// Operations pruned from the suites because they use bindings the generator
// does not implement yet (PLAN Phase 3b/4 follow-ups). This list, like the
// test exclusion list, must only shrink.
// alloy#simpleRestJson conformance: alloy's own protocol-test service
// (alloy-protocol-tests), the vendor-neutral counterpart of the AWS restJson1
// suite. Standard httpRequestTests/httpResponseTests, consumed as-is.
val generateSimpleRestJsonProtocolTests = registerProtocolTestTask(
    "generateSimpleRestJsonProtocolTests",
    "alloy.test#PizzaAdminService",
    "smithy::protocoltests::simplerestjson",
    "protocol-tests/simplerestjson/generated",
    omitOperations = listOf(
        // alloy-protocol-tests 0.3.32+: PrimitiveEncodings.duration targets
        // alloy#Duration, a bigDecimal — a type the generator rejects until
        // bigDecimal support lands.
        "alloy.test#Primitives",
    ),
    malformedTests = true,
)

val generateRpcv2CborProtocolTests = registerProtocolTestTask(
    "generateRpcv2CborProtocolTests",
    "smithy.protocoltests.rpcv2Cbor#RpcV2Protocol",
    "smithy::protocoltests::rpcv2cbor",
    "protocol-tests/rpcv2cbor/generated",
    malformedTests = true,
)

// jsonRpc2 conformance: our authored suite (JSON-RPC 2.0 is an open standard
// with no published Smithy protocol tests), checked in next to the generated
// module. Standard httpRequestTests/httpResponseTests/httpMalformedRequestTests.
val generateJsonRpc2ProtocolTests = registerProtocolTestTask(
    "generateJsonRpc2ProtocolTests",
    "smithy.cpp.protocoltests.jsonrpc2#JsonRpc2Protocol",
    "smithy::protocoltests::jsonrpc2",
    "protocol-tests/jsonrpc2/generated",
    malformedTests = true,
    modelPath = "protocol-tests/jsonrpc2/model/jsonrpc2.smithy",
)

tasks.register("generateProtocolTests") {
    group = "smithy-cpp"
    description = "Regenerates the checked-in protocol conformance suites under protocol-tests/"
    dependsOn(
        generateSimpleRestJsonProtocolTests,
        generateRpcv2CborProtocolTests,
        generateJsonRpc2ProtocolTests,
    )
}

tasks.register("generateFixtures") {
    group = "smithy-cpp"
    description = "Regenerates all checked-in generated code under examples/ (the goldens)"
    dependsOn(
        generateWeatherFixture,
        generateCafeFixture,
        generateChatFixture,
        generateBookstoreFixture,
        generateRoundTripRestFixture,
        generateRoundTripRpcFixture,
        generateRoundTripJsonRpcFixture,
        generateCalculatorFixture,
    )
}

tasks.withType<Test>().configureEach {
    systemProperty("smithycpp.repoRoot", repoRoot.absolutePath)
    // The golden-audit test cross-checks the checked-in protocol-test goldens
    // against the upstream suite definitions; the suite jars stay off the test
    // classpath (so ordinary tests don't assemble them) and are handed over as
    // paths instead.
    systemProperty("smithycpp.protocolTestModels", protocolTestModels.asPath)
    // Repo files the mirror tests read through smithycpp.repoRoot. Declared as
    // inputs so Gradle's test avoidance cannot report a stale green when only
    // these change — exactly what let PR #120 pass while its merge to main
    // failed: the PR's codegen job restored cached test results, the doc/dep
    // drift sat outside every declared input, and only main's fresh run
    // executed the tests (QuickstartMirrorTest caught it there).
    inputs.files(
        File(repoRoot, "docs/quickstart.md"),
        File(repoRoot, "examples/bazel-consumer/MODULE.bazel"),
        File(repoRoot, "examples/bazel-consumer/.bazelrc"),
        File(repoRoot, "examples/bazel-consumer/.bazelversion"),
        File(repoRoot, ".bazelversion"),
        File(repoRoot, "examples/bazel-consumer/todo_integration_test.cc"),
    ).withPathSensitivity(PathSensitivity.RELATIVE)
}

dependencies {
    api("software.amazon.smithy:smithy-codegen-core:1.73.0")
    api("software.amazon.smithy:smithy-build:1.73.0")
    // Trait definitions used by the fixture models.
    // alloy#simpleRestJson — the vendor-neutral REST/JSON protocol (smithy4s).
    // alloy is built against Smithy 1.58, which the whole build now pins to.
    implementation("com.disneystreaming.alloy:alloy-core:0.3.40")
    implementation("software.amazon.smithy:smithy-protocol-traits:1.73.0")
    // smithy.test#http{Request,Response}Tests trait definitions.
    implementation("software.amazon.smithy:smithy-protocol-test-traits:1.73.0")
    // The official conformance suite models (generateProtocolTests classpath only).
    // alloy's simpleRestJson conformance suite.
    protocolTestModels("com.disneystreaming.alloy:alloy-protocol-tests:0.3.40")
    protocolTestModels("software.amazon.smithy:smithy-protocol-tests:1.73.0")

    testImplementation(platform("org.junit:junit-bom:6.1.3"))
    testImplementation("org.junit.jupiter:junit-jupiter")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

// Local iteration helper (not for CI): prints the protocol-test runner classpath.
tasks.register("printProtocolTestClasspath") {
    doLast {
        println((sourceSets["main"].runtimeClasspath + protocolTestModels).asPath)
    }
}
