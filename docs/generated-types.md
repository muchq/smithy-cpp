# Generated types: the Smithy → C++ mapping contract

What `cpp-codegen` emits per shape (Phase 2: data types; Phase 3: serde + clients). This is a
compatibility contract: changes to it are breaking for consumers of generated code.

## Type mapping

| Smithy shape | C++ type | Notes |
|---|---|---|
| `boolean` | `bool` | |
| `byte` / `short` / `integer` / `long` | `std::int8_t` / `std::int16_t` / `std::int32_t` / `std::int64_t` | |
| `float` / `double` | `float` / `double` | |
| `string` | `std::string` | |
| `blob` | `smithy::Blob` | |
| `timestamp` | `smithy::Timestamp` | |
| `document` | `smithy::Document` | |
| `list<T>` | `std::vector<T>` | `@sparse` ⇒ `std::vector<std::optional<T>>` |
| `map<string, T>` | `std::map<std::string, T>` | `@sparse` ⇒ optional values; `std::map` keeps output deterministic |
| `structure` | `struct` with public members | Aggregate; `operator==` and `operator<=>` defaulted; every member value-initialized with `{}` |
| `union` | class over `std::variant` | See below |
| `enum` | class with nested `enum class Value` | See below; unknown wire values preserved |
| `intEnum` | `enum class X : std::int32_t` | Wire values outside int32 fail the parse; unknown in-range values are preserved (servers additionally validate membership) |
| `smithy.api#Unit` | `smithy::Unit` | Never declared; maps to the runtime type |
| `bigInteger` / `bigDecimal` | — | Rejected with a clear error (planned) |
| `@streaming` blob member | trait ignored | Generates as a fully buffered `smithy::Blob`; see the README's [Current limitations](../README.md#current-limitations) |
| `@streaming` union member | typed event stream (ADR-0016) | The operation generates `smithy::eventstream::EventStream` signatures (client and server) instead of carrying the union in the body; the union itself still generates as a normal union type |
| recursive structures | `smithy::Boxed<T>` member indirection | Deep copy/equality; list cycles ride `std::vector` directly. Cycles through union members or map values are still rejected with a clear error |

## Conventions

- **Names**: shape names are used as-is (PascalCase by Smithy convention); member names are used
  as-is (camelCase). C++ keywords get a trailing underscore (`namespace` → `namespace_`), and so
  do the two file-level identifiers generated sources claim for themselves: a *type* named
  `helpers` or `types` becomes `helpers_`/`types_` (generated `.cc` files keep their file-local
  helpers in a `helpers` namespace and reference model types through a `types` alias, so a model
  name can never shadow a helper or vice versa). Member names never need that escape.
- **Optionality**: `@required` members map to the plain type; everything else is
  `std::optional<T>` — except members with a non-null `@default` (and no `@clientOptional`),
  which are plain members initialized to the default, always serialized, and left at the
  default when absent from the wire. Members of `@input` structures stay client-optional per
  the spec (clients skip unset members; servers fill the default while parsing), and
  `@required` + `@default` reads absence as the default instead of failing.
- **Ordering**: generated types default `operator<=>` beside `operator==` whenever every member
  is three-way-comparable, so structs, unions, and enums key `std::map`/`std::set` and sort
  (issue #49). Types that can't order — a `smithy::Document` member, or recursion (via
  `smithy::Boxed`, which deliberately has no `<=>`: deducing a deep ordering around the cycle
  it exists to break is a hard error on clang), transitively through members — are
  equality-only: the generator omits `<=>` and leaves an "Equality-only" comment in the
  header. `float`/`double` members make the ordering partial.
- **Hashing**: a type specializes `std::hash` exactly when it gets `operator<=>`, so it keys
  `std::unordered_map`/`std::unordered_set` the same way it keys `std::map` (equality-only
  types get neither). Structs hash member-wise, enums hash their (value, unknown-text) pair,
  unions and `<Op>Errors` listings hash (engaged index, engaged member); list/map/optional
  members hash element-wise via `smithy::HashValue`, and the runtime types (`smithy::Blob`,
  `smithy::Timestamp`, `smithy::Unit`) carry `std::hash` in their own headers. The
  specializations sit after the namespace's closing brace in the same generated header.
  Hash values are **process-local**: they build on `std::hash`, so never persist them or
  compare them across processes, builds, or library versions.
- **Printing**: every generated type carries `AppendDebugTo(std::string&)` (the one printing
  primitive), with `DebugString()` and `operator<<` as thin adapters — so structs log and show
  up readably in test failures. Structs render designated-initializer style, omitting
  disengaged optionals (`GetOrderInput{.orderId = "o-1"}`); enums render their wire text,
  unknown values included (`CoffeeType(OAT_FOAM)`); unions and `<Op>Errors` render the engaged
  member (`MilkOption(dairy = DairyMilk{.percentFat = 2.5})`, empty ⇒ `MilkOption()`);
  intEnums render their numeric value. Members targeting a `@sensitive` shape print
  `[REDACTED]` — a printing feature that leaked sensitive values into logs would defeat the
  trait (issue #85; protobuf's `debug_redact` precedent). Unlike ordering/hashing, printing is
  **not gated**: recursion and `Document` members print fine (value semantics keep the data
  acyclic). The runtime member types render via the same mechanism: `Blob` as size plus a
  bounded hex prefix (never full contents), `Timestamp` as RFC 3339, `Document` JSON-ish,
  `smithy::DebugString(x)`/`DebugAppend` in `smithy/core/print.h` for anything else. Debug
  output is for **humans and logs only** — it is not a serialization format; never parse it,
  and never pin exact bytes across library versions.
- **Deliberately not generated**:
  - builders — C++20 designated initializers are the construction story:
    `GetOrderInput{.orderId = "o-1"}`;
  - `std::formatter` — deferred: `operator<<`/`DebugString()` cover logging and tests without
    putting `<format>`'s compile cost in every generated TU; when demanded, specializations
    slot into the same post-namespace epilogue `std::hash` uses;
  - a distinct "absent" state for `@required` members — they stay plain members; presence is
    enforced by server-side validation (see Optionality above).
- **Docs**: `@documentation` becomes `///` comments.
- **Files**: per module, `include/<namespace path>/types.h`, `serde.h` + `src/serde.cc`,
  `client.h` + `src/client.cc`, and a generated `BUILD.bazel` exposing `cc_library ":types"`
  and `":client"` targets that depend on the smithy-cpp runtime (`runtimeTarget` /
  `runtimePackage` settings).

## Enums

```cpp
class CoffeeType {
 public:
  enum class Value { kDrip, kEspresso, kUnknown };
  CoffeeType(Value value);                          // implicit, by design
  static CoffeeType FromString(std::string_view);   // unknown text => Value::kUnknown
  Value value() const;
  operator Value() const;                           // implicit: `switch (coffee)` works directly
  std::string_view ToString() const;                // unknown values keep their original text
  // == and <=> against CoffeeType; == against Value (keeps the implicit
  // conversion from making comparisons ambiguous).
};
```

Constants are `k` + PascalCase of the member name (`DRIP` → `kDrip`, `OAT_MILK` → `kOatMilk`).
Unknown-value preservation means a round trip through an old client never corrupts data written
by a newer service.

## Unions

```cpp
class MilkOption {
 public:
  MilkOption();                                   // empty() until a factory is used
  static MilkOption FromDairy(DairyMilk value);   // From + PascalCase(member)
  bool is_dairy() const;                          // is_<member>
  const DairyMilk& as_dairy() const;              // as_<member>; see below for the wrong-case contract
  const DairyMilk* as_dairy_or_null() const;      // engaged member or nullptr — never dies
  bool empty() const;
  const char* case_name() const;                  // engaged member's name; "(empty)" before any factory
  template <typename Visitor>
  decltype(auto) visit(Visitor&& visitor) const;  // std::visit over the members + std::monostate
};
```

Backed by `std::variant<std::monostate, ...members>` — index-addressed, so duplicate member
target types are fine.

Calling `as_x()` while a different member (or none) is engaged is a contract violation: it
terminates the process with the union, requested, and engaged member named (e.g.
`smithy: MilkOption::as_dairy(): engaged member is oat`) — never a context-free
`std::bad_variant_access`. For access that can't die, branch on `is_x()`, use
`as_x_or_null()` (`if (const auto* dairy = milk.as_dairy_or_null()) …`), or `visit()` with a
visitor that covers every member plus `std::monostate` for the empty state —
`smithy::Overloaded` (`smithy/core/overloaded.h`) builds one from lambdas.

## Serde (Phase 3)

`serde.h`/`src/serde.cc` emit one pair of free functions per aggregate shape reachable from an
operation:

```cpp
smithy::Document SerializeOrderCoffeeInput(const OrderCoffeeInput& value);
smithy::Outcome<OrderCoffeeInput> DeserializeOrderCoffeeInput(const smithy::Document& value);
```

- **Pivot type**: `smithy::Document` — protocol-independent; the client picks the JSON or CBOR
  codec at the wire boundary, so serde is generated once per shape, not once per protocol.
- **Tolerant reads**: unknown response members are ignored; unknown enum values are preserved
  (`Value::kUnknown` + original text). Missing `@required` members produce a
  `smithy::ErrorKind::kSerialization` error naming the member.
- **Sparse** lists/maps serialize `std::nullopt` as explicit nulls; timestamps honor
  `@timestampFormat` with the protocol default applied where unspecified.
- **alloy unions**: `@discriminated("key")` unions put the engaged member's fields inline with
  the discriminator spliced into the same object (`{"key": "smol", ...fields}`); a
  `@jsonUnknown` member (open unions, tagged or discriminated) retains the entire wire object
  when the tag or discriminator value matches no known member.
- **Union strictness and the `__type` exception**: a plain (tagged) union deserializes only a
  map with **exactly one** member key — empty maps, multiple engaged members, unknown member
  names, and explicit-null members are all `kSerialization` errors (ambiguous unions are a
  parser-differential hazard, so they never pass silently). The single deliberate exception:
  a `__type` key is excluded from that count, because error payloads carry the error shape's
  fully qualified id in `__type` right next to the payload members — a union member of an
  error structure therefore arrives with `__type` beside it. The union suites in
  `protocol-tests/unions/` pin both the strict rules and this tolerance.

## Clients (Phase 3)

`client.h`/`src/client.cc` emit a `<Service>Client` per service:

```cpp
// Create returns an Outcome (it validates config); value_or_die() unwraps it
// and, on failure, terminates with this context plus the error's code and
// message — a bare * works too, dying with the error alone.
auto client = WeatherClient::Create(std::move(config)).value_or_die("creating weather client");
auto city   = client.GetCity(GetCityInput{.cityId = "seattle"});  // Outcome<GetCityOutput>
```

- **Transport-agnostic**: `smithy::ClientConfig` supplies either an `endpoint` (uses the default
  socket transport) or an explicit `http_client` (loopback for tests, Beast, custom).
- **Protocol binding** is chosen at generation time from the service's protocol trait:
  simpleRestJson (HTTP bindings: labels, query, headers, status codes), rpcv2Cbor
  (`POST /service/{S}/operation/{O}`, `smithy-protocol: rpc-v2-cbor`, CBOR bodies), or
  jsonRpc2 (single `POST /`, `{"jsonrpc":"2.0","method":…,"params":…,"id":1}` envelopes).
- `@idempotencyToken` members are auto-filled with a UUIDv4 when unset; caller-provided values
  pass through untouched.
- HTTP 4xx/5xx map to `smithy::ErrorKind::kModeled` with the sanitized error code
  (`ns#Shape` → `Shape`, simpleRestJson also reads the `x-error-type` header; jsonRpc2 errors
  arrive as JSON-RPC error objects on HTTP 200, discriminated by `error.data.__type`);
  `@retryable` errors and 5xx responses (jsonRpc2: `error.code >= 500`) are marked retryable.
- **Typed errors**: when the code matches an error the operation declares, the deserialized
  error structure rides along as the error's detail. Every operation with modeled errors also
  gets a `<Operation>Errors` listing in `client.h` — the typed, typo-proof way to dispatch,
  with the same accessor surface as unions (`is_x`/`as_x`/`as_x_or_null`/`case_name`/`visit`):

  ```cpp
  const auto typed = GetOrderErrors::FromError(outcome.error());
  if (const auto* e = typed.as_order_not_found_or_null()) use(e->orderId);
  // or exhaustively — the visitor must also cover std::monostate, the
  // "not one of this operation's modeled errors" state:
  typed.visit(smithy::Overloaded{
      [](const OrderNotFound& e) { /* ... */ },
      [](std::monostate) { /* transport/serialization/unknown */ },
  });
  ```

  `FromError` matches only `kModeled` errors by code; an engaged member carries the
  deserialized detail (default-initialized if the error arrived without one). Undeclared codes
  still surface generically (code + message, no detail) and leave the listing `empty()`.
- simpleRestJson honors `@jsonName` body keys, serializes non-finite numbers as
  `"NaN"`/`"Infinity"`/`"-Infinity"`, and binds response `@httpHeader` members (including
  comma-joined lists and base64 `@mediaType` strings).

## Servers (Phase 4)

`server.h`/`src/server.cc` emit a pure-virtual `<Service>Handler` (one `Outcome`-returning
method per operation) and a `<Service>Server` that binds it to the runtime router; `Handler()`
returns a transport-agnostic `smithy::http::RequestHandler`. Routing, binding deserialization,
response serialization, and modeled-error mapping (`@httpError` status, `__type` body, typed
detail via `set_detail`) are generated — see [docs/server-guide.md](server-guide.md). Every
module also gets `tests/smoke_test.cc`: generated client ↔ generated server over loopback.

## Naming

- Cross-namespace name collisions in a service closure are disambiguated by appending the
  foreign namespace's last segment (`shared#Greeting` → `GreetingShared`); the service's own
  namespace keeps plain names, and wire-level error codes always use the Smithy shape name.
