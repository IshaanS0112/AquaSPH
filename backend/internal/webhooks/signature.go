// Package webhooks delivers job events to tenant endpoints from the transactional outbox
// (ADR-0005): at-least-once, signed, retried with backoff, dead-lettered after a bound.
package webhooks

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
)

// SignatureHeader carries "t=<unix seconds>,v1=<hex HMAC-SHA256 of "<t>.<body>">". Signing the
// timestamp with the body lets a receiver reject replays older than its tolerance.
const SignatureHeader = "AquaSPH-Signature"

// NewSecret returns a signing secret: "whsec_" and 32 random bytes, base64url.
func NewSecret() (string, error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return "whsec_" + base64.RawURLEncoding.EncodeToString(b), nil
}

// Sign returns the signature header value for body at time t.
func Sign(secret string, t time.Time, body []byte) string {
	ts := strconv.FormatInt(t.Unix(), 10)
	return "t=" + ts + ",v1=" + mac(secret, ts, body)
}

func mac(secret, ts string, body []byte) string {
	h := hmac.New(sha256.New, []byte(secret))
	h.Write([]byte(ts))
	h.Write([]byte{'.'})
	h.Write(body)
	return hex.EncodeToString(h.Sum(nil))
}

var (
	ErrBadSignature = errors.New("webhook signature does not match")
	ErrStale        = errors.New("webhook timestamp outside tolerance")
)

// Verify is what a receiver runs: it checks the HMAC in constant time and that the timestamp is
// within tolerance of now. Published for receivers written in Go, and used by the tests.
func Verify(secret, header string, body []byte, now time.Time, tolerance time.Duration) error {
	var ts, sig string
	for _, part := range strings.Split(header, ",") {
		k, v, _ := strings.Cut(strings.TrimSpace(part), "=")
		switch k {
		case "t":
			ts = v
		case "v1":
			sig = v
		}
	}
	unix, err := strconv.ParseInt(ts, 10, 64)
	if err != nil || sig == "" {
		return fmt.Errorf("%w: malformed header", ErrBadSignature)
	}
	if !hmac.Equal([]byte(sig), []byte(mac(secret, ts, body))) {
		return ErrBadSignature
	}
	if d := now.Sub(time.Unix(unix, 0)); d > tolerance || d < -tolerance {
		return ErrStale
	}
	return nil
}
