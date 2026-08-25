$version: "2.0"

namespace acme.todo

/// A deliberately small service: enough to show routing, bodies, constraint
/// validation, and a modeled error end to end.
///
/// Deliberately protocol-agnostic (the upstream Smithy way): the @http
/// bindings below describe HTTP semantics without picking a wire protocol.
/// The overlays next to this file bind the service to a concrete protocol
/// with `apply` — see bindings/simplerestjson.smithy, bindings/rpcv2cbor.smithy,
/// and bindings/jsonrpc2.smithy — so one model can serve REST and RPC without
/// being edited.
service Todo {
    version: "2026-01-01"
    operations: [AddTask, GetTask]
}

@http(method: "POST", uri: "/tasks")
operation AddTask {
    input := {
        @required
        @length(min: 1, max: 256)
        title: String

        priority: Priority

        effortHours: Float
    }

    output := {
        @required
        taskId: String

        @required
        title: String
    }
}

@readonly
@http(method: "GET", uri: "/tasks/{taskId}")
operation GetTask {
    input := {
        @required
        @httpLabel
        taskId: String
    }

    output := {
        @required
        taskId: String

        @required
        title: String

        done: Boolean
    }

    errors: [NoSuchTask]
}

/// int32 on the wire: out-of-range values fail the parse and unknown
/// in-range values fail server validation (#109) — the members above give
/// those checks out-of-tree, real-socket coverage.
intEnum Priority {
    LOW = 1
    HIGH = 2
}

@error("client")
@httpError(404)
structure NoSuchTask {
    @required
    message: String
}
