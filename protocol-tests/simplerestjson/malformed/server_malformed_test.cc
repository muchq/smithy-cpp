// Hand-written malformed-server suite for simpleRestJson (issue #48). The
// jsonrpc2 protocol tests generate an equivalent file from the model's
// httpMalformedRequestTests traits; the alloy conformance suite carries no
// such traits, so until upstream grows them this suite pins how the
// generated PizzaAdminService server rejects hostile requests — before the
// handler ever runs. Lives outside generated/ because that tree is a golden
// the codegen CI job regenerates byte-for-byte.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "smithy/json/json.h"
#include "smithy/protocoltests/simplerestjson/server.h"

namespace smithy::protocoltests::simplerestjson {
namespace {

// Counts invocations; a malformed request must be rejected before any
// operation runs, so every test asserts calls stays 0.
class RecordingHandler : public PizzaAdminServiceHandler {
 public:
  smithy::Outcome<AddMenuItemOutput> AddMenuItem(const AddMenuItemInput&,
                                                 const smithy::server::RequestContext&) override {
    ++calls;
    return AddMenuItemOutput{};
  }
  smithy::Outcome<CustomCodeOutput> CustomCode(const CustomCodeInput&,
                                               const smithy::server::RequestContext&) override {
    ++calls;
    return CustomCodeOutput{};
  }
  smithy::Outcome<GetEnumOutput> GetEnum(const GetEnumInput&,
                                         const smithy::server::RequestContext&) override {
    ++calls;
    return GetEnumOutput{};
  }
  smithy::Outcome<GetIntEnumOutput> GetIntEnum(const GetIntEnumInput&,
                                               const smithy::server::RequestContext&) override {
    ++calls;
    return GetIntEnumOutput{};
  }
  smithy::Outcome<GetMenuOutput> GetMenu(const GetMenuInput&,
                                         const smithy::server::RequestContext&) override {
    ++calls;
    return GetMenuOutput{};
  }
  smithy::Outcome<HeaderEndpointOutput> HeaderEndpoint(
      const HeaderEndpointInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return HeaderEndpointOutput{};
  }
  smithy::Outcome<HealthOutput> Health(const HealthInput&,
                                       const smithy::server::RequestContext&) override {
    ++calls;
    return HealthOutput{};
  }
  smithy::Outcome<HttpPayloadRequiredWithDefaultOutput> HttpPayloadRequiredWithDefault(
      const HttpPayloadRequiredWithDefaultInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return HttpPayloadRequiredWithDefaultOutput{};
  }
  smithy::Outcome<HttpPayloadWithDefaultOutput> HttpPayloadWithDefault(
      const HttpPayloadWithDefaultInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return HttpPayloadWithDefaultOutput{};
  }
  smithy::Outcome<OpenUnionsOutput> OpenUnions(const OpenUnionsInput&,
                                               const smithy::server::RequestContext&) override {
    ++calls;
    return OpenUnionsOutput{};
  }
  smithy::Outcome<PreserveOrderOutput> PreserveOrder(
      const PreserveOrderInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return PreserveOrderOutput{};
  }
  smithy::Outcome<RoundTripOutput> RoundTrip(const RoundTripInput&,
                                             const smithy::server::RequestContext&) override {
    ++calls;
    return RoundTripOutput{};
  }
  smithy::Outcome<VersionOutput> Version(const VersionInput&,
                                         const smithy::server::RequestContext&) override {
    ++calls;
    return VersionOutput{};
  }
  int calls = 0;
};

class SimpleRestJsonMalformedTest : public testing::Test {
 protected:
  smithy::http::HttpResponse Send(smithy::http::HttpRequest request) {
    return server_.Handler()(request);
  }

  std::shared_ptr<RecordingHandler> handler_ = std::make_shared<RecordingHandler>();
  PizzaAdminServiceServer server_{handler_};
};

TEST_F(SimpleRestJsonMalformedTest, UnknownRouteIs404) {
  smithy::http::HttpRequest request;
  request.method = "GET";
  request.target = "/no-such-route";
  EXPECT_EQ(Send(request).status, 404);
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(SimpleRestJsonMalformedTest, WrongMethodIs405) {
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/health";
  EXPECT_EQ(Send(request).status, 405);
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(SimpleRestJsonMalformedTest, UnparseableJsonBodyIsSerializationException) {
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/restaurant/r1/menu/item";
  request.headers.Set("content-type", "application/json");
  request.body = "{";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400) << response.body;
  EXPECT_EQ(response.headers.Get("x-error-type").value_or("<missing>"), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(SimpleRestJsonMalformedTest, WrongContentTypeIs415) {
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/restaurant/r1/menu/item";
  request.headers.Set("content-type", "text/plain");
  request.body = "{}";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 415) << response.body;
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(SimpleRestJsonMalformedTest, IntEnumLabelBeyondInt32IsRejectedBeforeTheHandler) {
  // The label parser bounds intEnum at int32 like Integer (issue #109's
  // serde-side fix pins the body path; this pins the text path).
  smithy::http::HttpRequest request;
  request.method = "GET";
  request.target = "/get-int-enum/99999999999";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400) << response.body;
  EXPECT_EQ(response.headers.Get("x-error-type").value_or("<missing>"), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(SimpleRestJsonMalformedTest, IntEnumLabelOutsideTheValueSetFailsValidation) {
  // In-range but unknown values fail membership validation with the
  // string-enum suite message, ints spelled the way smithy-rs emits them
  // (issue #109).
  smithy::http::HttpRequest request;
  request.method = "GET";
  request.target = "/get-int-enum/3";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400) << response.body;
  EXPECT_EQ(response.headers.Get("x-error-type").value_or("<missing>"), "ValidationException");
  EXPECT_EQ(handler_->calls, 0);

  const auto body = smithy::json::Decode(response.body);
  ASSERT_TRUE(body.ok()) << response.body;
  const smithy::Document* field_list = body->Find("fieldList");
  ASSERT_NE(field_list, nullptr) << response.body;
  ASSERT_EQ(field_list->as_list().size(), 1u) << response.body;
  const auto& failure = field_list->as_list()[0];
  EXPECT_EQ(failure.Find("path")->as_string(), "/aa");
  EXPECT_EQ(failure.Find("message")->as_string(),
            "Value at '/aa' failed to satisfy constraint: Member must satisfy enum value set: "
            "[1, 2]");
}

TEST_F(SimpleRestJsonMalformedTest, EnumLabelViolationReportsTheSuiteExactMessage) {
  smithy::http::HttpRequest request;
  request.method = "GET";
  request.target = "/get-enum/bogus";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400) << response.body;
  EXPECT_EQ(response.headers.Get("x-error-type").value_or("<missing>"), "ValidationException");
  EXPECT_EQ(handler_->calls, 0);

  const auto body = smithy::json::Decode(response.body);
  ASSERT_TRUE(body.ok()) << response.body;
  const smithy::Document* field_list = body->Find("fieldList");
  ASSERT_NE(field_list, nullptr) << response.body;
  ASSERT_EQ(field_list->as_list().size(), 1u) << response.body;
  const auto& failure = field_list->as_list()[0];
  EXPECT_EQ(failure.Find("path")->as_string(), "/aa");
  EXPECT_EQ(failure.Find("message")->as_string(),
            "Value at '/aa' failed to satisfy constraint: Member must satisfy enum value set: "
            "[v1, v2]");
}

}  // namespace
}  // namespace smithy::protocoltests::simplerestjson
