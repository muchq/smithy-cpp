# Working agreement

How work gets picked up, built, reviewed, and shipped in this repo. Written
down so a future session starts where the last one left off instead of
rediscovering the same conventions.

This is process, not architecture. Architecture lives in `docs/adr/`.

The same agreement is kept in MoonBase (`docs/WORKING_AGREEMENT.md`), and
improvements flow both ways. Where a convention there has no analogue here it
has been dropped rather than restated aspirationally, and where the tooling
differs this repo's command is the one named.

## Shipping a change

**One item, one PR — a default, not a law.** Work items come off a tracking
issue. Take the highest-severity open item, finish it, ship it, then take the
next. What the rule protects is a reviewer's ability to hold the whole change
at once, so judge a candidate against that rather than against a file count. It
binds hardest on genuinely independent work: two features, or a refactor riding
along with a fix.

A causal chain is one item. A fix, the regeneration it forces, and the golden
diff that follows cannot land separately — the generator change alone leaves
the checked-in goldens contradicting it, and CI fails on exactly that.

Work too small to be worth splitting is fine too: a one-line doc fix noticed in
passing does not need its own branch, review and CI cycle. And when you carry a
genuine second item because splitting would cost more than it saves, name it in
the PR body so the reviewer can ask for it to come out.

**Fold review feedback into the PR it came from.** When a review turns up
something small — a doc line that now contradicts itself, an assertion that
doesn't bite, a name that misleads — fix it in that PR. Don't file it. An issue
for a twenty-line fix costs more to write, triage, schedule and re-explain than
the fix does, and it lands on a reader who no longer has any of the context
that made the finding obvious.

This looks like a tension with "one item, one PR", and it resolves toward
folding, because the two rules protect against different costs and only one of
them is expensive here. Batching unrelated work makes a PR hard to review; that
is what the first rule is for. But a finding that came *out of* this review is
not unrelated to it — it is the review working. Splitting it out buys nothing
and spends the scarcest thing in the process: a reviewer who has the code
loaded right now.

Reach for a separate issue when the answer is genuinely unrelated to the change
under review, or when it is large enough to need its own design conversation.
"It wasn't in the original scope" is not one of them, and neither is "the
commit would touch a third file." When in doubt, fold it in and say in the PR
that you did.

**Altitude review first.** Before writing any code: read the cited code,
confirm the finding is actually real (several tracked items turned out to be
sharper *or* narrower than their description), and propose a plan. Only then
implement. Jumping straight to a fix hides the cases where the reported issue
is a symptom of something bigger.

**Ask about scope when the sizes differ materially.** If the plan has a
minimal version and a thorough version that lead to genuinely different work,
ask — with a recommendation, not a survey. If they only differ cosmetically,
pick the obvious one and say so.

**Question the request itself, not just how to build it.** Before implementing,
step back once and ask whether the framing is right. A request describes a
symptom the reporter noticed; it is not automatically the best response to that
symptom, and the person asking usually hasn't seen the constraint you're about
to read in the code.

Issue #130 is the worked example. It named a design question to settle first —
whether a timed-out receive should stash the late message or cancel the parked
one — and treated it as the hard part. Reading the transports dissolved it:
both already land inbound messages in per-session state and hand them to
whoever receives next, so timing out a *parked callback* releases the slot
without touching the wire, the read pump, or any in-flight message. There was
no stash to build and no cancellation primitive to invent. Answering the
question as posed would have added per-session state the socket layer already
had.

Raise the alternative in a sentence or two, give a recommendation, and proceed
— don't stall. If it turns out to be the better design, that is a much cheaper
discovery before the code exists than after.

**Don't open a PR unless asked.** Commit and push when the work is done; open
the PR only on request. Reference the tracking issue and, when the issue is a
checklist, tick the item once merged.

**Update the tracking issue.** Fold new data (reproductions, measurements,
scope corrections) back into the issue so it stays the source of truth. File
follow-ups for what you deliberately left out rather than leaving it implicit.

**What goes in commit messages, comments and PR bodies is one set of rules**,
and they live under "Writing it down" below.

## Review panel

Push the work — and open the PR, where one is being opened — then run a
self-review panel against that head.

Panelling before the first commit hides the step. Its findings get folded into
the same diff, so nothing in the PR says what the panel caught, what it got
wrong, or whether it ran at all — the reader is asked to take the claim on
trust. Opened first, every fix the panel produces is a commit on top of a
baseline CI has already judged, and the history is the evidence: this was
found, this changed because of it, this was reported and deliberately not acted
on.

It also gets the panel better inputs. The agents can read the PR body and the
CI result rather than a working tree, and a finding can be checked against a
known-green head instead of against a tree that has never been built anywhere
but here. Where no PR is being opened, the pushed branch is the baseline.

None of that licenses pushing a draft for the panel to finish. Push work you
would defend as it stands; the panel is the second opinion on a finished
change, not the first pass over an unfinished one.

The panel itself:

- **Four independent agents, four distinct lenses.** Typically correctness and
  control flow; concurrency, threading, and resource safety; tests, docs, and
  CI gates; and altitude. The lenses should barely overlap.
- **The altitude lens re-asks the pre-code question of the finished diff.** Is
  this change at the right level, or a patch over a symptom of something
  bigger? Does each new abstraction earn its keep, and would less code do? The
  other lenses stare at what the diff does; this one asks whether it should
  exist in this shape at all — the review most likely to be skipped, precisely
  because nothing is "wrong."
- **Panel agents read; they never write.** No edits, no "revert it and see what
  happens" — not even a change the agent fully intends to undo. The panel runs
  several agents at once over the same files, so one agent's scratch mutation
  is another's mystery failure; an agent that dies mid-run leaves deliberately
  broken code in the tree; and a dirty tree invites a commit that ships the
  mutation. An agent that wants to know whether a test bites reports that as a
  finding instead of finding out.
- **Enforce read-only structurally, not by instruction.** Convene panels on an
  agent type without edit or write tools, and keep write-shaped questions out
  of the briefs — "verify this test fails on the old code" is an instruction to
  mutate the tree no matter how firmly the same brief says never to.
- **Each agent hunts, then tries to refute its own findings** before reporting.
  This is what keeps the signal-to-noise usable.
- **Verify the survivors yourself** before acting on them. Agents are sometimes
  confidently wrong; don't take a finding at face value.
- **Aggregation is where the writing happens.** Every surviving finding not
  already covered gets a test — positive *and* negative — including the
  findings you decide *not* to act on, where the test pins the behavior you
  chose to keep so the next reader doesn't reopen the question. Mutation
  checking belongs here too: it needs a clean tree and a single writer.

The panel has earned its cost — it caught a real defect on several consecutive
PRs (an INT64_MIN decompose UB, a keep-alive framing gap, an unguarded
WebSocket upgrade target, two libraries missing from a new CI gate, and a
`std::thread` spawn that could throw beside a still-armed park, which from a
coroutine is a use-after-free).

**If the panel didn't run, say so.** A restart, an interrupt, or simply
forgetting can kill it. Report that plainly rather than letting the reader
assume the step happened.

**Answer review questions with tests, not paragraphs.** See below — this is
the single highest-leverage rule in this document.

**Look for simplification on every review.** See the next section: a review
that only hunts for defects is doing half the job.

## Design and simplification

**The goal is simple, readable code with clear interfaces.** Not clever code,
not maximally general code — code the next reader understands without a tour.
An interface that takes a paragraph to explain is a design problem wearing a
documentation problem's clothes; fix the interface.

**Testability is a core requirement, not a side effect.** If something is hard
to test, that is a design defect, and the design is what changes — never
settle for testing it badly, testing it indirectly, or not at all. The seams
that let a test drive the behavior (an injectable clock, a transport
interface, a callable policy) are part of the deliverable, not scaffolding
bolted on afterward.

**Every review is a simplification opportunity.** Alongside hunting defects,
ask: does this abstraction earn its keep? Can two near-identical paths become
one? Is this special case actually special? Can this be deleted outright? The
best review outcome is often less code, not more.

**Characterize before you refactor — positive *and* negative tests, first.**
Before changing the shape of existing code, cover it with tests that pin both
what it does and what it *refuses* to do, and confirm they pass against the
unchanged code. Only then refactor.

The ordering is the whole point. Tests written afterward describe the new
code's behavior, not the behavior you meant to preserve — that is how a
refactor silently becomes a rewrite. And the negative half is not optional:
positive tests alone let a refactor quietly *widen* behavior, accepting input
the original rejected, which is exactly the shape of a security regression.
Passing tests before and after are what make the change a refactor rather
than a hope.

## Testing bar

**A test beats an argument.** If a behavior is interesting enough to question,
debate, or reason carefully about — in a review, in a PR thread, or in your
own head — write a test that runs in CI instead. This is *always* better than
reasoning about correctness. Reasoning is invisible to the next reader, decays
as the code moves underneath it, and is exactly what the person who wrote the
bug already did. A test is executable, survives refactors, and fails at the
moment the property breaks rather than the moment someone notices.

In practice: when a reviewer asks "what happens if X?", the deliverable is a
CI test named after X — not a reply explaining why X is fine. When you catch
yourself constructing an argument for why something must be correct, stop and
write the test that would prove it.

**Comments are not a contract. CI tests are.** A doc comment stating a rule
constrains nothing. It is intent — and intent that nothing enforces drifts
from the code the moment someone edits without reading it, silently, with no
failure anywhere. "Comment as contract" is not an acceptable design; if a
property matters, something must *fail* when it is violated: a test, a type,
or a fail-fast check that a test then pins.

This repo has the receipts, and every one of them is an open item on the
conformance issue:

- `interceptor.h` documents that hooks "must not throw" — the virtuals are
  not `noexcept` and `retry.cc` calls them bare, so a throwing hook
  propagates straight out of a generated client operation.
- `timestamp.h` promises parsing with "no locale" — `ParseEpochSeconds`
  converts with `std::strtod`, which honors `LC_NUMERIC` and silently drops
  fractional milliseconds under a comma-decimal locale.
- `middleware.h` states "neither callback may be null" for `Guard` — nulls
  are accepted and every request 500s on `std::bad_function_call`.

Each one was true prose and false behavior for as long as nobody tested it.
Keep writing comments — they carry the *why*, which no test can — but the
comment documents the contract; it is never the contract itself.

**The Beyoncé Rule: if you liked it, you should have put a test on it.** Every
observable behavior worth keeping gets a test. Apply this liberally and at
every level that fits the behavior:

- **unit** — the mechanism itself,
- **integration** — the behavior through the real wire, transport, or codec,
- **out-of-tree consumer example** — where the behavior is part of the
  contract a consumer depends on, prove it through the module boundary the
  way a consumer would actually hit it. That means raw bytes and raw frames
  where the contract is a wire contract, not a round trip through generated
  types that regenerate on both sides and hide a rename.

An untested observable behavior is not a guarantee; it is a coincidence that
currently holds.

**Test through the same objects production uses.** A wire test that builds its
own serializer is testing the serializer it built. Pull the real one — the
generated router, the real transport, the production codec — and when "the real
one" is itself an inference, add one test that reads the actual bytes off a
real server. The consumer-side metrics test exists for exactly this: in-tree the
endpoint is driven by hand-written handlers that stamp the operation label
themselves, so those tests would keep passing if the generated router stopped
stamping it.

**TDD, nearly always — not just for bug fixes.** Write the test first, watch it
fail for the right reason, then write the code. This is the default for
features as much as for fixes; the exceptions are narrow (a spike you intend to
throw away, a pure rename) and "I already know what this does" is not one of
them.

The reason is design, not discipline. Writing the expectation first forces the
question "what should this do, and how would anyone tell?" while the answer is
still cheap to change — before an interface exists to be accommodated. Tests
written afterwards answer a different question: "what does this code do?" They
inherit the shape of whatever was built, including the parts that are awkward
to observe, and they are systematically blind to the case the implementation
forgot, because they were derived from it.

**Mutation checking is not a substitute for writing the test first.** It is
worth doing and it answers a genuinely useful question, but a much narrower
one: *does this assertion, as written, bite right now?* It cannot tell you the
assertion is the right one, and it cannot recover a case nobody thought to
assert, because it only mutates code that exists to break tests that exist.
Reaching for it to justify tests written after the fact is the failure it looks
most like a fix for.

**Mutation-test negative and security tests.** A test asserting that something
is *rejected* must be proven to fail when the property it pins is broken —
temporarily remove the check, confirm that exact test fails with its own
message, then restore. A test that passes for the wrong reason is worse than
no test, because it advertises coverage that isn't there. This is how the
client TLS hostname and version-floor tests were validated.

**Prove isolation with a control.** When a negative test asserts a failure,
add the positive twin that shares the fixture (e.g. the same hand-built
listener, one version higher) so a broken fixture can't masquerade as the
property holding.

**Fuzz targets for anything that parses.** Decoders, framing, URIs, headers,
compression. See `docs/fuzzing.md`.

**Consumer and e2e tests that flex the feature, not smoke tests.** New
functionality needs a test in the out-of-tree consumer module
(`examples/bazel-consumer`) that actually demonstrates it working through the
module boundary — the way a real consumer would use it.

**Re-run timing-sensitive tests.** Anything with threads or sockets gets
`--runs_per_test=15` or so before it's trusted.

**Watch for tests that don't actually run.** A `--test_filter` that matches
nothing exits green, and so does a suite whose new file never made it into
`srcs`. When a run "passes" the first time on a test you expected to be hard,
check the count: `--test_output=all` and read the `N tests from M test suites
ran` line before believing it.

## Verification before pushing

| Step | Command | Gated in CI? |
|---|---|---|
| C++ formatting | `clang-format` on every changed `.h`/`.cc` | yes — `lint` |
| BUILD formatting | `npx -y @bazel/buildifier@8.2.1 --lint=warn --mode=check -r .` | yes — `lint` |
| clang-tidy | `make tidy` | yes — `lint` |
| Runtime suite | `bazel test //...` | yes — `bazel (…)`, four toolchains |
| Lockfile freshness | `make lockfiles` | yes — `lockfiles` |
| Codegen + goldens | `make codegen goldens` | yes — `codegen (gradle)` |
| Sanitizers | `make sanitize`, plus tsan for concurrency | yes — `bazel (asan + ubsan, …)` |
| Exceptions-disabled gate | `make noexcept` | yes — `bazel (-fno-exceptions runtime)` |
| Consumer module | `bazel test //...` in `examples/bazel-consumer` | yes — `bazel consumer (…)` |
| Fuzz harnesses | `make fuzz-smoke` | yes — `fuzz (libFuzzer smoke)` |

`make verify` covers formatting, the runtime suite, lockfiles, and
codegen+goldens. `make verify-full` adds clang-tidy, the sanitizers, the
exceptions-disabled gate, the consumer module, and the fuzz harnesses. Note
that `make lint` shells out to a system `buildifier`, which the sandbox does
not have — the `npx` invocation above is the one CI uses and the one that runs
here.

**Check the exit code, not the tail of the output.** `make verify 2>&1 | tail`
reports *tail's* status, so a failed step scrolls past and the pipeline exits
0. That is how `make verify` got reported as passing twice in one session while
`lint` was aborting on a missing `buildifier` binary. Run the command bare, or
check `${PIPESTATUS[0]}`.

**CI is cheaper than model tokens.** The table is what CI will run, not a gate
every session must reproduce end to end before pushing. Run the fast checks —
the formatters, the tests beside the change — and push; a cold Bazel build of
half the repo costs more session time than the CI cycle it duplicates, and the
branch is where CI's answer lands anyway. This tunes economics, not honesty:
say exactly what ran locally and what is riding on CI, treat a red result as
work now, and never claim a step ran when it didn't. It also doesn't license
pushing what nothing checked — a change that never compiled anywhere is a
guess, not a candidate.

**New source files must be added to `srcs` and `hdrs` by name.** Nothing globs
here, so a file that isn't listed doesn't compile and its tests don't run.

**Be explicit about what couldn't be verified locally, and why.** The sandbox
has a pre-existing `rules_android` resolution failure in `//codegen`'s JVM
plugin that blocks consumer targets needing generated code, the proxy 403s
GitHub source archives that most BCR modules fetch (`bazel/make-git-overrides.sh`
rebuilds those from git clones — run it before concluding a target is
unbuildable here), and the clang sanitizer runtime is absent, so CI's
clang asan+ubsan combination can only be approximated with the gcc ones. When
you hit a limitation like that, *prove it's pre-existing* by reproducing it
with your changes stashed, then say so in the PR body.

## Docs and changelog

- Update docs in the same PR as the code: ADRs, guides, public header
  contract comments.
- Add a CHANGELOG entry.
- **When behavior changes, fix the doc that describes it in the same commit.**
  A doc left contradicting the code is a defect in its own right.
- **If a change alters an ADR's stated posture, amend the ADR.** ADR-0003 was
  amended when contract violations moved from `throw` to fail-fast, and again
  when recoverable config moved to an `Outcome`.
- Keep the claims accurate. Don't write that something is covered "everywhere"
  when a subtree is deliberately excluded; name the exclusion.

## Writing it down

**No archeology in comments.** A comment describes the code as it is, not how
it got there. No "used to", no "previously", no retelling of the bug that
prompted the line. Git has the history, and a comment narrating a deleted
alternative ages into a lie the moment someone edits around it.

A live trap is not archeology. `beast_transport.cc`'s "`empty_body`, not a
`string_body` with the body cleared out" earns its place because clearing the
body is the obvious wrong turn and the failure is a silent hang — that warns
about the code in front of you rather than recounting a previous attempt.

**A comment must not claim a property the code doesn't have.** A comment
describing the guarantee the author meant to build rather than the one that
shipped is worse than no comment: it makes a vacuous assertion look
deliberate. When the code changes underneath a comment, the comment is part of
the change.

**Comments are terse and present-tense.** A comment states a constraint the
code can't show, in a sentence or two. Keep the *why* — one line of why beats
five of history.

**Commit messages under 100 words, usually well under.** What changed and why,
in the fewest words that carry it; a one-line subject is often the whole job.
No narrated account of the session: no mutation-check kill lists, no "written
before the change and observed red", no confession that the panel didn't run.
That material is real, and its home is the PR body or the tests.

This repo merges with merge commits, so every branch commit keeps its own
message in `git log` forever. A bloated message is bloat in the history of
every future `git log` and `git blame` that walks through it. If the short
paragraph keeps growing, that is a sign the *change* should have been split,
not that the message needs more room.

**Terse PR bodies.** The change, the consequences a reviewer cannot see from
the diff, and what is deliberately not covered. Nothing else. The body is
spent entirely on reviewer attention, which is the scarcest thing in the
process.

**No journaling in any artifact.** "My first attempt", "this turned out to
be", "I then found" — none of that belongs in code, commit messages, or PR
bodies. A finding from a review lives in the review thread; the artifact
carries only the conclusion.

## Dependencies and infrastructure

**Re-check assumed limitations instead of repeating them.** A limitation
recorded in a previous session may no longer hold — the Bazel sandbox
restriction was re-examined and turned out to be workable via git-based module
overrides.

**When you find a workaround, make it reusable.** Document it in the repo docs
and add a setup script so the next session gets it for free
(`bazel/make-git-overrides.sh`, wired to a SessionStart hook).

**Dependency bumps: don't trust the PR's own green CI.** Check how stale its
base is. A renovate PR whose checks passed against a base 50 commits old has
validated a tree that no longer exists. Merge it into current `main` locally
and run the affected suites — that's the only signal that matters. For a
security-sensitive dependency, also ask what the existing tests actually
*assert* (a posture test that checks the negotiated cipher is worth far more
than one that checks the connection succeeded).

**A bump that clears an advisory may need more than the version number.** Read
the advisory's affected range against the available versions rather than taking
a proposed bump at face value; an in-range bump sometimes cannot clear the
advisory at all.

**Bumps have fallout beyond compilation.** A build that still succeeds is not
the whole answer — run what the change touches, not just its tests.

## Communication

- Raise a concern in a sentence or two, then proceed with the work. Don't
  stop and wait unless proceeding would be unsafe or wasted.
- Report outcomes faithfully: if a step was skipped, say it; if tests failed,
  show it.
- Distinguish real defects from nits when reviewing, and say which is which.
- Don't re-litigate decisions already made.

## Operational notes

- **Merge commits on the working branch.** After a PR merges, the branch is
  reset onto `main`, which brings GitHub's own merge commit (committer
  `noreply@github.com`) along. Tooling may flag it as unverified; it is
  already-published upstream history and must not be amended.
- **Wedged PRs.** The owner sometimes merges locally and pushes `main`
  directly, leaving the PR object open with phantom conflicts. Before
  believing a conflict, check whether the PR head is already an ancestor of
  `origin/main`; a push to the branch un-wedges it.
- **Never commit `MODULE.bazel.lock` churn** produced by the sandbox module
  overrides — they run under `--lockfile_mode=off` for exactly that reason,
  and `make lockfiles` is what CI checks the real lockfiles against.
