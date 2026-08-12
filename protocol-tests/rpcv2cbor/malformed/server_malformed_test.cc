// Hand-written malformed-server suite for rpcv2Cbor (issue #48). The
// official Smithy conformance suite carries no httpMalformedRequestTests for
// this protocol, so this pins how the generated RpcV2Protocol server rejects
// hostile requests — protocol preconditions, unparseable CBOR, bad routing —
// before the handler runs. Lives outside generated/ because that tree is a
// golden the codegen CI job regenerates byte-for-byte.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "smithy/cbor/cbor.h"
#include "smithy/protocoltests/rpcv2cbor/server.h"
#include "smithy/testing/protocol_test.h"

namespace smithy::protocoltests::rpcv2cbor {
namespace {

class RecordingHandler : public RpcV2ProtocolHandler {
 public:
  smithy::Outcome<EmptyInputOutputOutput> EmptyInputOutput(
      const EmptyInputOutputInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return EmptyInputOutputOutput{};
  }
  smithy::Outcome<Float16Output> Float16(const Float16Input&,
                                         const smithy::server::RequestContext&) override {
    ++calls;
    return Float16Output{};
  }
  smithy::Outcome<FractionalSecondsOutput> FractionalSeconds(
      const FractionalSecondsInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return FractionalSecondsOutput{};
  }
  smithy::Outcome<GreetingWithErrorsOutput> GreetingWithErrors(
      const GreetingWithErrorsInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return GreetingWithErrorsOutput{};
  }
  smithy::Outcome<NoInputOutputOutput> NoInputOutput(
      const NoInputOutputInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return NoInputOutputOutput{};
  }
  smithy::Outcome<OperationWithDefaultsOutput> OperationWithDefaults(
      const OperationWithDefaultsInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return OperationWithDefaultsOutput{};
  }
  smithy::Outcome<OptionalInputOutputOutput> OptionalInputOutput(
      const OptionalInputOutputInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return OptionalInputOutputOutput{};
  }
  smithy::Outcome<RecursiveShapesOutput> RecursiveShapes(
      const RecursiveShapesInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return RecursiveShapesOutput{};
  }
  smithy::Outcome<RpcV2CborDenseMapsOutput> RpcV2CborDenseMaps(
      const RpcV2CborDenseMapsInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return RpcV2CborDenseMapsOutput{};
  }
  smithy::Outcome<RpcV2CborListsOutput> RpcV2CborLists(
      const RpcV2CborListsInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return RpcV2CborListsOutput{};
  }
  smithy::Outcome<RpcV2CborSparseMapsOutput> RpcV2CborSparseMaps(
      const RpcV2CborSparseMapsInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return RpcV2CborSparseMapsOutput{};
  }
  smithy::Outcome<RpcV2CborUnionsOutput> RpcV2CborUnions(
      const RpcV2CborUnionsInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return RpcV2CborUnionsOutput{};
  }
  smithy::Outcome<SimpleScalarPropertiesOutput> SimpleScalarProperties(
      const SimpleScalarPropertiesInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return SimpleScalarPropertiesOutput{};
  }
  smithy::Outcome<SparseNullsOperationOutput> SparseNullsOperation(
      const SparseNullsOperationInput&, const smithy::server::RequestContext&) override {
    ++calls;
    return SparseNullsOperationOutput{};
  }
  int calls = 0;
};

// ::testing, not testing: inside namespace smithy::*, protocol_test.h's
// smithy::testing shadows gtest's global namespace for unqualified lookup.
class RpcV2CborMalformedTest : public ::testing::Test {
 protected:
  smithy::http::HttpRequest WellFormedRequest(const std::string& operation) {
    return smithy::testing::Rpcv2CborRequest("RpcV2Protocol", operation);
  }

  smithy::http::HttpResponse Send(const smithy::http::HttpRequest& request) {
    return server_.Handler()(request);
  }

  // The protocol serializes errors as a CBOR map carrying __type.
  std::string ErrorTypeOf(const smithy::http::HttpResponse& response) {
    EXPECT_EQ(response.headers.Get("smithy-protocol").value_or("<missing>"), "rpc-v2-cbor");
    EXPECT_EQ(response.headers.Get("content-type").value_or("<missing>"), "application/cbor");
    const auto body = smithy::cbor::Decode(Blob::FromString(response.body));
    EXPECT_TRUE(body.ok());
    if (!body.ok() || !body->is_map()) return "<unparseable>";
    const smithy::Document* type = body->Find("__type");
    return type == nullptr ? "<missing>" : std::string(type->as_string());
  }

  std::shared_ptr<RecordingHandler> handler_ = std::make_shared<RecordingHandler>();
  RpcV2ProtocolServer server_{handler_};
};

TEST_F(RpcV2CborMalformedTest, MissingSmithyProtocolHeaderIsRejected) {
  auto request = WellFormedRequest("NoInputOutput");
  request.headers.Remove("smithy-protocol");
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorTypeOf(response), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, WrongSmithyProtocolHeaderIsRejected) {
  auto request = WellFormedRequest("NoInputOutput");
  request.headers.Set("smithy-protocol", "rpc-v2-json");
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorTypeOf(response), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, WrongContentTypeIs415) {
  auto request = WellFormedRequest("SimpleScalarProperties");
  request.headers.Set("content-type", "application/json");
  request.body = "{}";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 415);
  EXPECT_EQ(ErrorTypeOf(response), "UnsupportedMediaTypeException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, TruncatedCborBodyIsSerializationException) {
  auto request = WellFormedRequest("SimpleScalarProperties");
  request.body = "\x18";  // uint8 header, argument byte missing
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorTypeOf(response), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, NonMapCborBodyIsSerializationException) {
  auto request = WellFormedRequest("SimpleScalarProperties");
  request.body = "\x01";  // a bare integer where a structure map is required
  const auto response = Send(request);
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(ErrorTypeOf(response), "SerializationException");
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, UnknownOperationIs404) {
  const auto response = Send(WellFormedRequest("NoSuchOperation"));
  EXPECT_EQ(response.status, 404);
  EXPECT_EQ(handler_->calls, 0);
}

TEST_F(RpcV2CborMalformedTest, WrongMethodIs405) {
  auto request = WellFormedRequest("NoInputOutput");
  request.method = "GET";
  const auto response = Send(request);
  EXPECT_EQ(response.status, 405);
  EXPECT_EQ(handler_->calls, 0);
}

}  // namespace
}  // namespace smithy::protocoltests::rpcv2cbor
