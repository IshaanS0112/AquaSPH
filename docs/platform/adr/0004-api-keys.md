# ADR-0004: API keys, hashed, with a lookup prefix

**Status:** accepted

## Decision
Keys look like `aqk_<8-char prefix>_<32-char secret>`. They are 190 bits of
entropy from `crypto/rand`. Postgres stores the prefix (unique, indexed) and
`sha256(full key)`. Authentication looks the key up by prefix, then compares
hashes with `subtle.ConstantTimeCompare`.

## Why SHA-256 and not bcrypt/argon2
bcrypt exists to slow down guessing *low-entropy human passwords*. A 190-bit
random secret cannot be brute-forced whatever the hash speed, so a slow hash
only adds ~50 ms of CPU to every API request. This is the same reasoning
GitHub and Stripe apply to their tokens.

## Why not JWT
The clients are machines: notebooks, CI, services. JWTs are useful when a
verifier cannot reach the issuer's database. Here every request already hits
Postgres. JWTs cannot be revoked before they expire without a denylist, and
a denylist is a database lookup anyway.

## Consequences
- A leaked database dump does not leak usable keys.
- Revocation is immediate (`revoked_at`).
- The plaintext key is shown once, at creation, by `aquasph-admin`.
