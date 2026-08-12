// Protocol binding overlay: pairs with model/redirector.smithy to bind the
// protocol-agnostic Redirector service to simpleRestJson. Pass both files to
// the generation rule; the base model never mentions a protocol.
$version: "2.0"

namespace acme.redirect

use alloy#simpleRestJson

apply Redirector @simpleRestJson
