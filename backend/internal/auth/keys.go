// Package auth implements API keys (ADR-0004).
//
// Format: aqk_<prefix>_<secret>
//
//	prefix  8 chars of [a-z0-9]    lookup handle, stored in plaintext, safe to log
//	secret  32 chars of base62      ~190 bits from crypto/rand
//
// Only sha256(full key) is stored. Because the secret is high-entropy
// random data rather than a human password, a fast hash is correct here;
// bcrypt would add ~50 ms per request and buy nothing.
package auth

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"errors"
	"math/big"
	"strings"
)

const (
	keyPrefix   = "aqk_"
	prefixLen   = 8
	secretLen   = 32
	prefixAlpha = "abcdefghijklmnopqrstuvwxyz0123456789"
	secretAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
	fullKeyLen  = len(keyPrefix) + prefixLen + 1 + secretLen
)

var ErrMalformedKey = errors.New("malformed API key")

func randomString(alphabet string, n int) (string, error) {
	var b strings.Builder
	max := big.NewInt(int64(len(alphabet)))
	for i := 0; i < n; i++ {
		// rand.Int draws uniformly; indexing with a random byte modulo
		// len(alphabet) would bias toward the first 256 % 62 characters.
		idx, err := rand.Int(rand.Reader, max)
		if err != nil {
			return "", err
		}
		b.WriteByte(alphabet[idx.Int64()])
	}
	return b.String(), nil
}

// Generated is a newly minted key. Plaintext is shown to the operator once
// and never stored.
type Generated struct {
	Plaintext string
	Prefix    string
	Hash      []byte
}

func Generate() (Generated, error) {
	prefix, err := randomString(prefixAlpha, prefixLen)
	if err != nil {
		return Generated{}, err
	}
	secret, err := randomString(secretAlpha, secretLen)
	if err != nil {
		return Generated{}, err
	}
	full := keyPrefix + prefix + "_" + secret
	return Generated{Plaintext: full, Prefix: prefix, Hash: Hash(full)}, nil
}

// Hash is the stored form of a key.
func Hash(full string) []byte {
	sum := sha256.Sum256([]byte(full))
	return sum[:]
}

// Parse validates the shape of a presented key and returns its prefix.
// Shape checks happen before any database lookup, so garbage in the
// Authorization header costs nothing.
func Parse(full string) (prefix string, err error) {
	if len(full) != fullKeyLen || !strings.HasPrefix(full, keyPrefix) {
		return "", ErrMalformedKey
	}
	prefix = full[len(keyPrefix) : len(keyPrefix)+prefixLen]
	if full[len(keyPrefix)+prefixLen] != '_' {
		return "", ErrMalformedKey
	}
	for _, c := range prefix {
		if !strings.ContainsRune(prefixAlpha, c) {
			return "", ErrMalformedKey
		}
	}
	for _, c := range full[len(keyPrefix)+prefixLen+1:] {
		if !strings.ContainsRune(secretAlpha, c) {
			return "", ErrMalformedKey
		}
	}
	return prefix, nil
}

// Verify compares a presented key against a stored hash in constant time.
func Verify(full string, storedHash []byte) bool {
	return subtle.ConstantTimeCompare(Hash(full), storedHash) == 1
}
