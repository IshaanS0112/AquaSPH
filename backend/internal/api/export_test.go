package api

// RoutePatterns exposes the registered patterns to the OpenAPI drift test.
func RoutePatterns() []string {
	s := &Server{}
	var out []string
	for _, r := range s.routes() {
		out = append(out, r.pattern)
	}
	return out
}
