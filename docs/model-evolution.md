# Model evolution: the day-2 loop

[quickstart.md](quickstart.md) gets a model generating a client and server; this guide covers
everything after that — the loop you run every time the model changes. The shape of the loop
depends on where generated code lives, and there are exactly two answers:

| Mode | Generated code lives | Drift possible? | The loop |
|---|---|---|---|
| **In-graph** (the Bazel rules, recommended) | inside the build graph, never on disk | No — every build regenerates | edit model → `bazel test //...` → fix what the compiler names |
| **Vendored** (CLI output checked into git; also this repo's own `examples/*/generated` and `protocol-tests/`) | in the repository | Yes — model and output can diverge | edit model → regenerate → review the diff → commit both together |

## The in-graph loop

With `smithy_cpp_{types,client,server}_library` ([quickstart.md](quickstart.md) §3), the model
is the only source of truth: generation is a hermetic action, so an edited `.smithy` file
invalidates exactly the targets that depend on it. There is nothing to regenerate by hand and
nothing to drift. The compiler is the review:

- **Adding an optional member** is source-compatible. Existing handlers, call sites, and tests
  rebuild unchanged; the new member shows up as a `std::optional<T>` (or `@default`-initialized)
  field on the generated struct.
- **Adding an operation** grows the generated `<Service>Handler` interface by one pure-virtual
  method, so every handler implementation stops compiling until it implements the new
  operation. The unimplemented surface is found by the compiler, not by a 404 in production.
- **Renaming or removing** anything shows up the same way: every call site that mentioned the
  old name fails to compile. Rename-refactors are grep-follow-the-errors sessions, not
  archaeology.

Both properties are pinned in CI:
[`examples/bazel-consumer/model-evolution-check.sh`](../examples/bazel-consumer/model-evolution-check.sh)
scripts the loop against the out-of-tree consumer module — stage 1 adds an optional member and
requires `bazel test //...` to stay green untouched; stage 2 adds a `ListTasks` operation and
requires the integration test to *fail compilation* naming the new handler method. The
consumer CI job runs it on every commit, so the promises in this section cannot silently rot.

## The vendored loop (regenerate → diff → review → commit)

Vendoring generated sources (the CLI escape hatch in [quickstart.md](quickstart.md)
"Generating outside Bazel") trades the in-graph guarantees for a plain source tree, and takes
on one obligation: **the model and its generated output must change in the same commit.** This
repo's own fixtures live this way — `examples/*/generated/` and `protocol-tests/*/generated/`
are checked in as goldens — so the workflow below is exercised on every commit here.

1. **Edit the model** (`examples/<name>/model/*.smithy`, or your own).
2. **Regenerate.** In this repo, one command per fixture or all at once:

   ```sh
   cd codegen
   gradle generateWeatherFixture       # one fixture…
   gradle generateFixtures generateProtocolTests   # …or everything checked in
   ```

   Out of tree, the same generator runs as a CLI:

   ```sh
   bazel run @smithy_cpp//codegen:generator -- \
       --model $PWD/model/todo.smithy --service acme.todo#Todo \
       --namespace acme::todo --mode both --output $PWD/generated
   ```

   The Gradle tasks delete the output directory before regenerating, so *removals* show up as
   deleted lines in the diff rather than orphaned files; do the same for vendored output
   (`rm -rf generated/ && bazel run …`).
3. **Review the diff as a normal diff.** Generation is byte-deterministic (enforced by
   `CppCodegenPluginTest.outputIsByteDeterministic`), so the diff is exactly the blast radius
   of the model change — no noise, no timestamps. What to look for:
   - `include/**/types.h` — the API surface consumers compile against. New members and
     operations should look additive; anything *removed* here is a breaking change to your
     consumers ([versioning.md](versioning.md) §"What counts as the compatibility surface").
   - `src/serde.cc`, `src/client.cc`, `src/server.cc` — the wire behavior. Internals may
     reshuffle freely between generator versions (explicitly not a compatibility surface), but
     for a pure model edit with a pinned generator the changes should mention only the shapes
     you touched.
   - `tests/` — regenerated integration/smoke suites pick up new members automatically.
4. **Build and test**, then **commit the model and generated output together.**

## Drift detection in CI

A vendored tree needs CI to fail fast when someone edits a model without regenerating (or
edits generated files by hand). The pattern is: regenerate, then require a clean tree. This
repo's `codegen` job does precisely that for the checked-in goldens:

```yaml
- name: check generated fixtures are current
  run: |
    (cd codegen && gradle generateFixtures generateProtocolTests)
    git diff --exit-code -- examples protocol-tests
```

A consumer vendoring CLI output wants the same job, substituting the CLI invocation:

```yaml
- name: check generated code is current
  run: |
    rm -rf generated
    bazel run @smithy_cpp//codegen:generator -- \
        --model $PWD/model/todo.smithy --service acme.todo#Todo \
        --namespace acme::todo --mode both --output $PWD/generated
    git diff --exit-code -- generated
```

Two conventions that keep the check honest:

- **Pin the generator.** Byte-identical regeneration is only guaranteed for a pinned generator
  version (`git_override` commit / release tag); across generator upgrades the conformance
  suites are the contract instead ([versioning.md](versioning.md) §"What a generator upgrade
  may change"). Upgrading the pin regenerates everything in one reviewable commit.
- **Exempt generated code from format checks.** Its shape is locked by the drift check; this
  repo's lint job skips `*/generated/*` for clang-format/clang-tidy rather than fighting the
  generator.

## Worked example: adding a field to the weather fixture

The smallest real evolution, run against this repo. Add `chanceOfSnow` to `GetForecast`'s
output in `examples/weather/model/weather.smithy`:

```smithy
    output := {
        chanceOfRain: Float

        chanceOfSnow: Float
    }
```

Regenerate that one fixture and look at the blast radius:

```sh
(cd codegen && gradle generateWeatherFixture)
git diff --stat -- examples/weather
#  .../generated/include/example/weather/types.h  |  1 +
#  .../generated/src/serde.cc                     | 15 +++++++++++++++
#  .../generated/src/server.cc                    |  3 +++
#  .../generated/tests/integration_test.cc        |  1 +
#  .../model/weather.smithy                       |  2 ++
```

22 added lines, nothing removed. The interesting hunk is the API surface:

```diff
 struct GetForecastOutput {
   std::optional<float> chanceOfRain{};
+  std::optional<float> chanceOfSnow{};

   friend bool operator==(const GetForecastOutput&, const GetForecastOutput&) = default;
 };
```

plus the matching serialize/deserialize clauses in `serde.cc`/`server.cc` and one line in the
regenerated random-round-trip integration test. `bazel test //examples/weather/...` passes
with every hand-written test untouched — additive members are invisible to code that does not
use them. Commit `weather.smithy` and `examples/weather/generated/` together and the drift
check stays green.

## Which model changes are safe?

Wire compatibility (old peers keep working) and source compatibility (existing C++ keeps
compiling) are separate questions:

| Model change | Wire | Source (consumers of generated headers) |
|---|---|---|
| Add optional member | ✅ old readers ignore it; absent → unset `std::optional` | ✅ additive |
| Add member with `@default` | ✅ absent → the default | ✅ additive (plain member, default-initialized) |
| Add operation | ✅ old clients never call it | ⚠️ handlers must implement the new pure-virtual method (compile error guides) |
| Add enum value | ✅ old *clients* preserve unknown values (string enums: `Value::kUnknown` + original text; intEnums keep the in-range value) — ⚠️ old *servers* reject values outside their modeled set with 400 `ValidationException` | ✅ additive |
| Promote optional → `@required` + `@default` | ✅ absence on the wire keeps the default (the generator's evolution leniency) | ✅ member becomes plain (non-optional) — call sites reading `.has_value()` need updating |
| Promote optional → `@required` (no default) | ❌ old writers that omit it now fail deserialization | ❌ member type changes |
| Rename member | ❌ wire key changes — unless the old wire name is kept via `@jsonName` | ❌ compile errors at every use |
| Remove member / operation | ❌ breaking | ❌ breaking |
| Tighten a constraint (`@length`, `@pattern`, `@range`) | ⚠️ previously-valid requests now rejected with 400 `ValidationException` before the handler runs | ✅ no header change |

For anything in the ❌ rows, [versioning.md](versioning.md) applies: it is a breaking change to
your service's consumers, and (for changes to this repo's generator itself) must be called out
in the CHANGELOG under a "Breaking" heading with a migration note.

## Streaming unions

Adding a member to a `@streaming` event union is a wire-breaking change
for live peers: an unknown `:event-type` is a terminal serialization
error on the receiving side (the closed-union posture, applied to
streams), so old peers' sessions end at the first new-member event.
Roll out receivers before senders, or model a new operation.
