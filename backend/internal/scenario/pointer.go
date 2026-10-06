// Package scenario resolves what a job will run: it loads the scenario catalogue, applies JSON
// Pointer overrides, expands sweep grids, and computes the canonical hash the result cache is
// keyed on.
package scenario

import (
	"fmt"
	"strconv"
	"strings"
)

// Pointer is a parsed RFC 6901 JSON Pointer.
type Pointer []string

// ParsePointer parses an RFC 6901 pointer. The root pointer "" is
// rejected: replacing a whole document is what an inline spec is for.
func ParsePointer(s string) (Pointer, error) {
	if s == "" {
		return nil, fmt.Errorf("the root pointer is not allowed; submit an inline spec instead")
	}
	if !strings.HasPrefix(s, "/") {
		return nil, fmt.Errorf("must start with '/'")
	}
	raw := strings.Split(s[1:], "/")
	out := make(Pointer, len(raw))
	for i, tok := range raw {
		// ~1 before ~0, per RFC 6901 section 4, so "~01" decodes to "~1".
		if strings.Contains(strings.ReplaceAll(strings.ReplaceAll(tok, "~0", ""), "~1", ""), "~") {
			return nil, fmt.Errorf("invalid escape in %q", tok)
		}
		out[i] = strings.ReplaceAll(strings.ReplaceAll(tok, "~1", "/"), "~0", "~")
	}
	return out, nil
}

func (p Pointer) String() string {
	var b strings.Builder
	for _, tok := range p {
		b.WriteByte('/')
		b.WriteString(strings.ReplaceAll(strings.ReplaceAll(tok, "~", "~0"), "/", "~1"))
	}
	return b.String()
}

func arrayIndex(tok string, n int) (int, error) {
	// RFC 6901: decimal, no leading zeros, and "-" (past the end) is not a
	// value that can be read or replaced.
	if tok == "" || (len(tok) > 1 && tok[0] == '0') {
		return 0, fmt.Errorf("invalid array index %q", tok)
	}
	i, err := strconv.Atoi(tok)
	if err != nil || i < 0 {
		return 0, fmt.Errorf("invalid array index %q", tok)
	}
	if i >= n {
		return 0, fmt.Errorf("array index %d out of range (length %d)", i, n)
	}
	return i, nil
}

// Get returns the value at p in doc.
func (p Pointer) Get(doc any) (any, error) {
	cur := doc
	for depth, tok := range p {
		switch node := cur.(type) {
		case map[string]any:
			v, ok := node[tok]
			if !ok {
				return nil, fmt.Errorf("%s does not exist", p[:depth+1])
			}
			cur = v
		case []any:
			i, err := arrayIndex(tok, len(node))
			if err != nil {
				return nil, fmt.Errorf("%s: %w", p[:depth+1], err)
			}
			cur = node[i]
		default:
			return nil, fmt.Errorf("%s is not an object or array", p[:depth])
		}
	}
	return cur, nil
}

// Replace sets the value at p, which must already exist. See Override for
// why creating new members is not allowed.
func (p Pointer) Replace(doc any, value any) error {
	parent, err := p[:len(p)-1].Get(doc)
	if err != nil {
		return err
	}
	last := p[len(p)-1]
	switch node := parent.(type) {
	case map[string]any:
		if _, ok := node[last]; !ok {
			return fmt.Errorf("%s does not exist", p)
		}
		node[last] = value
	case []any:
		i, err := arrayIndex(last, len(node))
		if err != nil {
			return fmt.Errorf("%s: %w", p, err)
		}
		node[i] = value
	default:
		return fmt.Errorf("%s is not an object or array", p[:len(p)-1])
	}
	return nil
}
