#include "smithy/server/middleware.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "smithy/http/loopback.h"
#include "smithy/http/trace_context.h"

namespace smithy::server {
namespace {

using std::chrono::milliseconds;

http::HttpResponse Ok(const std::string& body) {
  http::HttpResponse response;
  response.status = 200;
  response.body = body;
  return response;
}

// Middleware appending a tag on the way in (request seen) and out (response).
Middleware Tag(std::vector<std::string>* log, const std::string& name) {
  return [log, name](http::RequestHandler next) {
    return [log, name, next](const http::HttpRequest& request) {
      log->push_back(name + ":in");
      http::HttpResponse response = next(request);
      log->push_back(name + ":out");
      return response;
    };
  };
}

TEST(ChainTest, FirstMiddlewareIsOutermost) {
  std::vector<std::string> log;
  auto handler = Chain({Tag(&log, "a"), Tag(&log, "b")}, [&](const http::HttpRequest&) {
    log.push_back("handler");
    return Ok("done");
  });

  const auto response = handler({});
  EXPECT_EQ(response.body, "done");
  EXPECT_EQ(log, (std::vector<std::string>{"a:in", "b:in", "handler", "b:out", "a:out"}));
}

TEST(ChainTest, EmptyChainIsTheHandler) {
  auto handler = Chain({}, [](const http::HttpRequest&) { return Ok("plain"); });
  EXPECT_EQ(handler({}).body, "plain");
}

TEST(ChainTest, MiddlewareCanShortCircuit) {
  bool reached = false;
  auto reject = [](http::RequestHandler) {
    return [](const http::HttpRequest&) {
      http::HttpResponse response;
      response.status = 401;
      return response;
    };
  };
  auto handler = Chain({reject}, [&](const http::HttpRequest&) {
    reached = true;
    return Ok("never");
  });

  EXPECT_EQ(handler({}).status, 401);
  EXPECT_FALSE(reached);
}

TEST(ObserveTest, ReportsMethodTargetStatusAndDuration) {
  std::vector<RequestObservation> observations;
  auto clock_time = std::chrono::steady_clock::time_point{};
  // Each call advances by 750µs: sub-millisecond latencies (cache hits,
  // loopback) must survive — a milliseconds field would report 0 and zero
  // out exactly the fast-path histogram buckets (migration feedback).
  auto now = [&clock_time] {
    auto current = clock_time;
    clock_time += std::chrono::microseconds(750);
    return current;
  };

  auto handler = Chain(
      {Observe([&](const RequestObservation& o) { observations.push_back(o); }, nullptr, now)},
      [](const http::HttpRequest&) {
        http::HttpResponse response;
        response.status = 404;
        return response;
      });

  http::HttpRequest request;
  request.method = "GET";
  request.target = "/cities/1";
  (void)handler(request);

  ASSERT_EQ(observations.size(), 1u);
  EXPECT_EQ(observations[0].method, "GET");
  EXPECT_EQ(observations[0].target, "/cities/1");
  EXPECT_EQ(observations[0].status, 404);
  EXPECT_EQ(observations[0].duration, std::chrono::microseconds(750));
}

TEST(ObserveTest, TracesAreNeverEmptyWhenServedThroughATransport) {
  // ADR-0011: the transport ingress (server_dispatch.h) mints a root
  // traceparent when the client sent none, so an Observe composed under any
  // transport reports a parseable trace identity for every request. (Only a
  // handler chain driven directly in tests sees an empty trace_parent.)
  std::vector<RequestObservation> observations;
  auto handler = Chain({Observe([&observations](const RequestObservation& observation) {
                         observations.push_back(observation);
                       })},
                       [](const http::HttpRequest&) { return http::HttpResponse{}; });

  http::Loopback loopback;
  ASSERT_TRUE(loopback.Start(handler).ok());
  http::HttpRequest request;  // deliberately no traceparent
  ASSERT_TRUE(loopback.Send(request).ok());
  ASSERT_EQ(observations.size(), 1u);
  EXPECT_TRUE(http::ParseTraceparent(observations[0].trace_parent).has_value())
      << observations[0].trace_parent;
}

TEST(ObserveTest, OnStartFiresBeforeDispatch) {
  std::vector<std::string> log;
  auto handler = Chain({Observe([&](const RequestObservation&) { log.push_back("complete"); },
                                [&](const RequestStart& s) {
                                  log.push_back("start:" + s.method + " " + s.target);
                                })},
                       [&](const http::HttpRequest&) {
                         log.push_back("handler");
                         return Ok("done");
                       });

  http::HttpRequest request;
  request.method = "POST";
  request.target = "/tasks";
  (void)handler(request);
  EXPECT_EQ(log, (std::vector<std::string>{"start:POST /tasks", "handler", "complete"}));
}

TEST(ObserveTest, PairsCompleteWithStartWhenDispatchThrows) {
  int started = 0;
  std::vector<RequestObservation> completions;
  auto handler = Chain({Observe([&](const RequestObservation& o) { completions.push_back(o); },
                                [&](const RequestStart&) { ++started; })},
                       [](const http::HttpRequest&) -> http::HttpResponse {
                         throw std::runtime_error("handler exploded");
                       });

  EXPECT_THROW((void)handler({}), std::runtime_error);  // containment stays upstream
  EXPECT_EQ(started, 1);
  ASSERT_EQ(completions.size(), 1u);  // an in-flight gauge never leaks
  EXPECT_EQ(completions[0].status, 500);
  EXPECT_EQ(completions[0].operation, "");
  EXPECT_EQ(completions[0].method, "GET");
  EXPECT_EQ(completions[0].target, "/");
}

TEST(ObserveTest, DispatchThrowRethrowsOriginalEvenIfOnCompleteThrows) {
  auto handler =
      Chain({Observe([](const RequestObservation&) { throw std::runtime_error("metrics down"); },
                     [](const RequestStart&) {})},
            [](const http::HttpRequest&) -> http::HttpResponse {
              throw std::logic_error("handler exploded");
            });
  EXPECT_THROW((void)handler({}), std::logic_error);  // original survives, not runtime_error
}

TEST(ObserveTest, ThrowingOnStartIsContainedAndTheRequestIsServed) {
  auto handler =
      Chain({Observe([](const RequestObservation&) {},
                     [](const RequestStart&) { throw std::runtime_error("gauge backend down"); })},
            [](const http::HttpRequest&) { return Ok("served"); });
  http::HttpResponse response;
  EXPECT_NO_THROW(response = handler({}));
  EXPECT_EQ(response.body, "served");
}

TEST(RequireBearerAuthTest, ValidatesTheBearerToken) {
  auto handler = Chain({RequireBearerAuth([](const std::string& token) { return token == "s3"; })},
                       [](const http::HttpRequest&) { return Ok("in"); });

  http::HttpRequest request;
  EXPECT_EQ(handler(request).status, 401);  // no header

  request.headers.Set("authorization", "Bearer s3");
  EXPECT_EQ(handler(request).status, 200);

  request.headers.Set("authorization", "bearer s3");  // scheme is case-insensitive
  EXPECT_EQ(handler(request).status, 200);

  request.headers.Set("authorization", "Bearer nope");
  EXPECT_EQ(handler(request).status, 401);

  request.headers.Set("authorization", "Basic s3");  // wrong scheme
  EXPECT_EQ(handler(request).status, 401);

  request.headers.Set("authorization", "Bearer");  // scheme without credential
  EXPECT_EQ(handler(request).status, 401);
}

TEST(RequireApiKeyHeaderTest, ValidatesTheNamedHeader) {
  auto handler = Chain(
      {RequireApiKeyHeader("x-api-key", "", [](const std::string& key) { return key == "k"; })},
      [](const http::HttpRequest&) { return Ok("in"); });

  http::HttpRequest request;
  EXPECT_EQ(handler(request).status, 401);
  request.headers.Set("x-api-key", "k");
  EXPECT_EQ(handler(request).status, 200);
  request.headers.Set("x-api-key", "wrong");
  EXPECT_EQ(handler(request).status, 401);
}

TEST(RequireApiKeyHeaderTest, SchemePrefixesTheKey) {
  auto handler = Chain({RequireApiKeyHeader("authorization", "ApiKey",
                                            [](const std::string& key) { return key == "k"; })},
                       [](const http::HttpRequest&) { return Ok("in"); });

  http::HttpRequest request;
  request.headers.Set("authorization", "ApiKey k");
  EXPECT_EQ(handler(request).status, 200);
  request.headers.Set("authorization", "k");  // missing scheme
  EXPECT_EQ(handler(request).status, 401);
}

TEST(ObserveTest, CountsEveryRequest) {
  int count = 0;
  auto handler = Chain({Observe([&](const RequestObservation&) { ++count; })},
                       [](const http::HttpRequest&) { return Ok("ok"); });
  (void)handler({});
  (void)handler({});
  (void)handler({});
  EXPECT_EQ(count, 3);
}

TEST(ObserveDeathTest, NullOnCompleteAbortsAtComposition) {
  // A composition-time contract violation fails fast (ADR-0009), so no
  // exception crosses the boundary and the runtime builds -fno-exceptions.
  EXPECT_DEATH(Observe(nullptr), "smithy::server::Observe: on_complete may not be null");
}

TEST(ObserveTest, ThrowingCallbackDoesNotDiscardResponseOrPropagate) {
  auto handler = Chain({Observe([](const RequestObservation&) {
                         throw std::runtime_error("metrics backend down");
                       })},
                       [](const http::HttpRequest&) { return Ok("payload"); });
  http::HttpResponse response;
  EXPECT_NO_THROW(response = handler({}));
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body, "payload");
}

TEST(GuardTest, AdmittedRequestsPassThrough) {
  auto handler = Chain({Guard([](const http::HttpRequest&) { return true; }, TooManyRequests())},
                       [](const http::HttpRequest&) { return Ok("served"); });
  const auto response = handler({});
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body, "served");
}

TEST(GuardTest, RejectedRequestsShortCircuitWithTheRejectResponse) {
  bool reached = false;
  auto handler = Chain({Guard([](const http::HttpRequest&) { return false; },
                              [](const http::HttpRequest& request) {
                                http::HttpResponse response;
                                response.status = 503;
                                response.body = "maintenance: " +
                                                request.headers.Get("x-request-id").value_or("");
                                return response;
                              })},
                       [&](const http::HttpRequest&) {
                         reached = true;
                         return Ok("never");
                       });
  http::HttpRequest request;
  request.headers.Set("x-request-id", "r-42");
  const auto response = handler(request);
  EXPECT_EQ(response.status, 503);
  EXPECT_EQ(response.body, "maintenance: r-42");
  EXPECT_FALSE(reached);
}

TEST(GuardTest, AdmitSeesTheRequest) {
  // The rate-limiting instantiation: admit keys on a header.
  auto handler =
      Chain({Guard(
                [](const http::HttpRequest& request) {
                  return request.headers.Get("x-forwarded-for").value_or("") != "10.0.0.1";
                },
                TooManyRequests())},
            [](const http::HttpRequest&) { return Ok("in"); });

  http::HttpRequest allowed;
  allowed.headers.Set("x-forwarded-for", "10.0.0.2");
  EXPECT_EQ(handler(allowed).status, 200);

  http::HttpRequest limited;
  limited.headers.Set("x-forwarded-for", "10.0.0.1");
  EXPECT_EQ(handler(limited).status, 429);
}

TEST(PerClientRateLimitTest, KeysOnTheDerivedClientBehindTheTrustBoundary) {
  // The issue #104 mutant killers, upstreamed: allow must see the DERIVED
  // client — two forwarded clients behind one trusted proxy are two
  // distinct keys (a raw-peer or ignored-trust mutant hands every proxied
  // request the proxy's single key and passes any test that doesn't look).
  std::vector<std::string> seen;
  auto handler = Chain({PerClientRateLimit(
                           [&seen](const std::string& client) {
                             seen.push_back(client);
                             return client != "203.0.113.9";
                           },
                           *http::TrustedProxies::Parse({"10.0.0.0/8"}), std::chrono::seconds(7))},
                       [](const http::HttpRequest&) { return Ok("in"); });

  http::HttpRequest first;
  first.peer_address = "10.0.0.1:443";
  first.headers.Set("x-forwarded-for", "198.51.100.7");
  EXPECT_EQ(handler(first).status, 200);

  http::HttpRequest second = first;
  second.headers.Set("x-forwarded-for", "198.51.100.7, 203.0.113.9");
  const auto limited = handler(second);
  EXPECT_EQ(limited.status, 429);
  EXPECT_EQ(limited.headers.Get("retry-after").value_or(""), "7");
  EXPECT_EQ(limited.body, R"({"error":"Too many requests"})");  // the shaped reject, not a copy

  // Every derivable source is consulted, with the bare port-stripped key:
  // a direct (untrusted) peer, the same peer with its spoofed header
  // ignored, and a trusted peer that sent no header — which keys as the
  // tier's own address, the shared-bucket consequence middleware.h
  // documents for a proxy that stops appending x-forwarded-for.
  http::HttpRequest direct;
  direct.peer_address = "203.0.113.9:52814";
  EXPECT_EQ(handler(direct).status, 429);

  http::HttpRequest spoofing = direct;
  spoofing.headers.Set("x-forwarded-for", "198.51.100.99");
  EXPECT_EQ(handler(spoofing).status, 429);

  http::HttpRequest tier;
  tier.peer_address = "10.0.0.1:443";
  EXPECT_EQ(handler(tier).status, 200);

  ASSERT_EQ(seen.size(), 5u);
  EXPECT_EQ(seen[0], "198.51.100.7");
  EXPECT_EQ(seen[1], "203.0.113.9");
  EXPECT_EQ(seen[2], "203.0.113.9");
  EXPECT_EQ(seen[3], "203.0.113.9");
  EXPECT_EQ(seen[4], "10.0.0.1");
}

TEST(PerClientRateLimitTest, TheDefaultRejectCarriesNoRetryAfter) {
  // The nullopt default must thread through to TooManyRequests unchanged,
  // not become some fallback interval.
  auto handler = Chain(
      {PerClientRateLimit([](const std::string&) { return false; }, http::TrustedProxies::None())},
      [](const http::HttpRequest&) { return Ok("in"); });
  http::HttpRequest request;
  request.peer_address = "203.0.113.9:52814";
  const auto limited = handler(request);
  EXPECT_EQ(limited.status, 429);
  EXPECT_FALSE(limited.headers.Has("retry-after"));
}

TEST(PerClientRateLimitTest, AnUnknownClientIsAdmittedWithoutConsultingAllow) {
  // No derivable peer (Loopback, hand-driven chains): admitting without a
  // key beats sharing one "" bucket across everything unkeyable. The
  // header alone must not conjure a key.
  bool consulted = false;
  auto handler = Chain({PerClientRateLimit(
                           [&consulted](const std::string&) {
                             consulted = true;
                             return false;
                           },
                           *http::TrustedProxies::Parse({"10.0.0.0/8"}))},
                       [](const http::HttpRequest&) { return Ok("in"); });

  http::HttpRequest unkeyable;
  unkeyable.headers.Set("x-forwarded-for", "203.0.113.9");
  EXPECT_EQ(handler(unkeyable).status, 200);
  EXPECT_FALSE(consulted);
}

TEST(PerClientRateLimitDeathTest, ANullPolicyAbortsAtComposition) {
  EXPECT_DEATH(PerClientRateLimit(nullptr, http::TrustedProxies::None()),
               "smithy::server::PerClientRateLimit: allow must not be null");
}

TEST(TooManyRequestsTest, ShapesThe429) {
  const auto response = TooManyRequests()(http::HttpRequest{});
  EXPECT_EQ(response.status, 429);
  EXPECT_EQ(response.headers.Get("content-type").value_or(""), "application/json");
  EXPECT_EQ(response.body, R"({"error":"Too many requests"})");
  EXPECT_FALSE(response.headers.Has("retry-after"));
}

TEST(TooManyRequestsTest, SetsRetryAfterWhenGiven) {
  const auto response = TooManyRequests(std::chrono::seconds(30))(http::HttpRequest{});
  EXPECT_EQ(response.status, 429);
  EXPECT_EQ(response.headers.Get("retry-after").value_or(""), "30");
}

TEST(HealthEndpointTest, AnswersGetOnThePath) {
  bool reached = false;
  auto handler = Chain({HealthEndpoint()}, [&](const http::HttpRequest&) {
    reached = true;
    return Ok("router");
  });

  http::HttpRequest request;
  request.method = "GET";
  request.target = "/health";
  const auto response = handler(request);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.headers.Get("content-type").value_or(""), "application/json");
  EXPECT_EQ(response.body, R"({"status":"healthy"})");
  EXPECT_FALSE(reached);
}

TEST(HealthEndpointTest, LabelsItsResponseWithItsOwnPath) {
  // Without this the probe reports as the empty operation, which is what a
  // 404 reports too — so a metrics backend cannot tell a liveness probe from
  // a request for a route that does not exist.
  auto live =
      Chain({HealthEndpoint("/livez")}, [](const http::HttpRequest&) { return Ok("router"); });
  auto ready = Chain({HealthEndpoint("/readyz", {{"db", [] { return false; }}})},
                     [](const http::HttpRequest&) { return Ok("router"); });

  http::HttpRequest request;
  request.method = "GET";
  request.target = "/livez";
  EXPECT_EQ(live(request).operation, "/livez");

  // The 503 path is labeled too: an unhealthy probe is the one you most need
  // to find on a dashboard.
  request.target = "/readyz";
  const auto unhealthy = ready(request);
  EXPECT_EQ(unhealthy.status, 503);
  EXPECT_EQ(unhealthy.operation, "/readyz");

  // Two instances on one server stay distinguishable rather than collapsing
  // into a single "health" bucket, and a HEAD is labeled like its GET.
  http::HttpRequest head;
  head.method = "HEAD";
  head.target = "/livez";
  EXPECT_EQ(live(head).operation, "/livez");
  head.target = "/readyz";
  EXPECT_EQ(ready(head).operation, "/readyz");
}

TEST(HealthEndpointTest, LeavesTheOperationToTheRouterOnPassThrough) {
  auto handler = Chain({HealthEndpoint()}, [](const http::HttpRequest&) {
    http::HttpResponse response;
    response.operation = "GetThing";
    return response;
  });

  http::HttpRequest request;
  request.method = "GET";
  request.target = "/things/1";
  EXPECT_EQ(handler(request).operation, "GetThing");
}

TEST(HealthEndpointTest, IgnoresTheQueryString) {
  auto handler = Chain({HealthEndpoint()}, [](const http::HttpRequest&) { return Ok("router"); });
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/health?verbose=1";
  EXPECT_EQ(handler(request).body, R"({"status":"healthy"})");
}

TEST(HealthEndpointTest, PassesThroughOtherMethodsAndTargets) {
  auto handler = Chain({HealthEndpoint()}, [](const http::HttpRequest&) { return Ok("router"); });

  http::HttpRequest post;
  post.method = "POST";
  post.target = "/health";
  EXPECT_EQ(handler(post).body, "router");  // the router decides (404/405/route)

  http::HttpRequest other;
  other.method = "GET";
  other.target = "/healthz";
  EXPECT_EQ(handler(other).body, "router");

  http::HttpRequest prefixed;
  prefixed.method = "GET";
  prefixed.target = "/healthx";
  EXPECT_EQ(handler(prefixed).body, "router");
}

TEST(HealthEndpointTest, CustomPath) {
  auto handler = Chain({HealthEndpoint("/status/live")},
                       [](const http::HttpRequest&) { return Ok("router"); });
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/status/live";
  EXPECT_EQ(handler(request).status, 200);

  request.target = "/health";
  EXPECT_EQ(handler(request).body, "router");
}

TEST(HealthEndpointTest, ReadinessPassesWhenEveryCheckPasses) {
  auto handler = Chain(
      {HealthEndpoint("/readyz", {{"db", [] { return true; }}, {"cache", [] { return true; }}})},
      [](const http::HttpRequest&) { return Ok("router"); });
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/readyz";
  const auto response = handler(request);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body, R"({"status":"healthy"})");
}

TEST(HealthEndpointTest, ReadinessIs503NamingEveryFailingCheck) {
  auto handler = Chain({HealthEndpoint("/readyz", {{"db", [] { return false; }},
                                                   {"cache", [] { return true; }},
                                                   {"queue", [] { return false; }}})},
                       [](const http::HttpRequest&) { return Ok("router"); });
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/readyz";
  const auto response = handler(request);
  EXPECT_EQ(response.status, 503);
  EXPECT_EQ(response.headers.Get("content-type").value_or(""), "application/json");
  EXPECT_EQ(response.body, R"({"status":"unhealthy","failing":["db","queue"]})");
}

TEST(HealthEndpointTest, ReadinessProbesRunPerRequestNotOnce) {
  // A cached answer would keep saying 200 while a dependency is down.
  bool ready = true;
  auto handler = Chain({HealthEndpoint("/readyz", {{"db", [&] { return ready; }}})},
                       [](const http::HttpRequest&) { return Ok("router"); });
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/readyz";
  EXPECT_EQ(handler(request).status, 200);
  ready = false;
  EXPECT_EQ(handler(request).status, 503);
  ready = true;
  EXPECT_EQ(handler(request).status, 200);
}

TEST(HealthEndpointTest, AThrowingProbeIsAFailingCheckNotAnUnwind) {
  auto handler = Chain(
      {HealthEndpoint("/readyz", {{"db", []() -> bool { throw std::runtime_error("down"); }}})},
      [](const http::HttpRequest&) { return Ok("router"); });
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/readyz";
  const auto response = handler(request);
  EXPECT_EQ(response.status, 503);
  EXPECT_EQ(response.body, R"({"status":"unhealthy","failing":["db"]})");
}

TEST(HealthEndpointTest, AThrowingProbeLogsTheCheckNameAndReason) {
  // The exception message is the one clue to why /readyz is flapping; the
  // 503 body names the check but containment must not discard the why.
  auto handler = Chain(
      {HealthEndpoint("/readyz",
                      {{"db", []() -> bool { throw std::runtime_error("connection refused"); }}})},
      [](const http::HttpRequest&) { return Ok("router"); });
  http::HttpRequest request;
  request.method = "GET";
  request.target = "/readyz";

  std::ostringstream log;
  std::streambuf* previous = std::clog.rdbuf(log.rdbuf());
  (void)handler(request);
  std::clog.rdbuf(previous);

  EXPECT_NE(log.str().find("readiness probe 'db'"), std::string::npos) << log.str();
  EXPECT_NE(log.str().find("connection refused"), std::string::npos) << log.str();
}

TEST(HealthEndpointDeathTest, NullProbeAbortsAtComposition) {
  // Contained per-request, a null probe would read as a permanent outage, so
  // it fails fast at composition (ADR-0009).
  EXPECT_DEATH(HealthEndpoint("/readyz", {{"db", nullptr}}), "has a null probe");
}

TEST(HealthEndpointDeathTest, NameThatWouldCorruptTheJsonAbortsAtComposition) {
  // Names land verbatim between the failing list's quotes; reject at
  // composition time what would produce invalid JSON during an outage.
  for (const std::string name : {"d\"b", "d\\b", "d\nb"}) {
    EXPECT_DEATH(HealthEndpoint("/readyz", {{name, [] { return true; }}}),
                 "contains a quote, backslash, or control character")
        << name;
  }
  EXPECT_NO_THROW(HealthEndpoint("/readyz", {{"db:primary/us-east 1", [] { return true; }}}));
}

TEST(HealthEndpointTest, ReadinessAnswersHeadWithTheStatusAndTheGetsBody) {
  auto handler = Chain({HealthEndpoint("/readyz", {{"db", [] { return false; }}})},
                       [](const http::HttpRequest&) { return Ok("router"); });
  http::HttpRequest request;
  request.method = "HEAD";
  request.target = "/readyz";
  const auto response = handler(request);
  EXPECT_EQ(response.status, 503);
  EXPECT_EQ(response.body, R"({"status":"unhealthy","failing":["db"]})");
}

TEST(HealthEndpointTest, AnswersHeadWithWhatTheGetWouldCarry) {
  // The middleware does not strip the body for HEAD, because the length it
  // reports is the answer a HEAD is asking for. The transport withholds the
  // octets and keeps that length; a handler that emptied the body here would
  // reduce it to Content-Length: 0. Pinned on the wire by
  // BeastTransportTest.TheHealthEndpointsHeadReportsTheGetsLength.
  bool reached = false;
  auto handler = Chain({HealthEndpoint()}, [&](const http::HttpRequest&) {
    reached = true;
    return Ok("router");
  });

  http::HttpRequest head;
  head.method = "HEAD";
  head.target = "/health";
  http::HttpRequest get;
  get.method = "GET";
  get.target = "/health";
  const auto response = handler(head);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.headers.Get("content-type").value_or(""), "application/json");
  EXPECT_EQ(response.body, handler(get).body);
  EXPECT_EQ(response.body, R"({"status":"healthy"})");
  EXPECT_FALSE(reached);
}

}  // namespace
}  // namespace smithy::server
