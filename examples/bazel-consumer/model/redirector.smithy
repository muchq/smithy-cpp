$version: "2.0"

namespace acme.redirect

/// A URL shortener's core shape: resolve a slug, answer 3xx with Location and
/// no useful body. Kept separate from Todo because a redirect is HTTP-specific
/// — @httpResponseCode and a Location header mean nothing over an RPC binding,
/// so this service binds to simpleRestJson only (bindings/redirector.smithy).
///
/// The two operations are the two spellings of the same wire response, and
/// both are here on purpose: they take different branches through the
/// generator, and only one of them used to work.
service Redirector {
    version: "2026-01-01"
    operations: [Resolve, ResolveDynamic, Fetch]
}

/// The status is fixed, so it rides the @http trait.
///
/// @suppress is load-bearing: Smithy's own HttpResponseCodeSemantics validator
/// fails the model with "Expected an `http` code in the 2xx range, but found
/// 302" — a DANGER event that stops model assembly. Redirects are the
/// legitimate exception, and there is no narrower way to say so.
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

    errors: [NoSuchSlug]
}

/// The status is chosen per request instead: a retired slug moves permanently
/// (301), a live one temporarily (302). @httpResponseCode carries it, so the
/// modeled @http code stays 200 and no suppression is needed.
@readonly
@http(method: "GET", uri: "/d/{slug}")
operation ResolveDynamic {
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

    errors: [NoSuchSlug]
}

/// A conditional fetch: 200 with the content, or 304 with none. The other
/// half of the no-content rule — here the body is an @httpPayload rather than
/// a document, which the generator writes on a different branch, so a guard
/// that only covered document bodies would still put content on the 304.
@readonly
@http(method: "GET", uri: "/c/{slug}")
operation Fetch {
    input := {
        @required
        @httpLabel
        slug: String

        @httpHeader("If-None-Match")
        ifNoneMatch: String
    }

    output := {
        @required
        @httpResponseCode
        status: Integer

        @httpHeader("ETag")
        etag: String

        @httpPayload
        content: Blob
    }

    errors: [NoSuchSlug]
}

@error("client")
@httpError(404)
structure NoSuchSlug {
    @required
    message: String
}
