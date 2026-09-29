// Package apispec embeds the OpenAPI description of the HTTP API so the server can serve it and
// tests can check it against the registered routes.
package apispec

import _ "embed"

//go:embed openapi.yaml
var OpenAPI []byte
