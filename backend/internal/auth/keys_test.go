package auth

import (
	"strings"
	"testing"
)

func TestGeneratedKeysParseAndVerify(t *testing.T) {
	seen := map[string]bool{}
	for i := 0; i < 200; i++ {
		g, err := Generate()
		if err != nil {
			t.Fatal(err)
		}
		if seen[g.Plaintext] {
			t.Fatal("duplicate key")
		}
		seen[g.Plaintext] = true
		p, err := Parse(g.Plaintext)
		if err != nil || p != g.Prefix {
			t.Fatalf("Parse(%q) = %q, %v", g.Plaintext, p, err)
		}
		if !Verify(g.Plaintext, g.Hash) {
			t.Fatal("key does not verify against its own hash")
		}
	}
}

func TestVerifyRejectsAnyOtherKey(t *testing.T) {
	a, _ := Generate()
	b, _ := Generate()
	if Verify(b.Plaintext, a.Hash) {
		t.Fatal("wrong key verified")
	}
	// Same prefix, different secret: the case an attacker who learned a
	// prefix from a log line would try.
	forged := a.Plaintext[:len(a.Plaintext)-1] + "x"
	if forged == a.Plaintext {
		forged = a.Plaintext[:len(a.Plaintext)-1] + "y"
	}
	if Verify(forged, a.Hash) {
		t.Fatal("forged secret verified")
	}
}

func TestParseRejectsMalformedKeys(t *testing.T) {
	g, _ := Generate()
	bad := []string{
		"",
		"Bearer " + g.Plaintext,
		strings.Replace(g.Plaintext, "aqk_", "sk_", 1),
		g.Plaintext + "x",
		g.Plaintext[:len(g.Plaintext)-1],
		strings.Replace(g.Plaintext, "_", "-", 2),
		"aqk_ABCDEFGH_" + strings.Repeat("a", 32), // prefix must be lowercase
		"aqk_abcdefgh_" + strings.Repeat("!", 32),
	}
	for _, k := range bad {
		if _, err := Parse(k); err == nil {
			t.Errorf("Parse(%q) accepted", k)
		}
	}
}
