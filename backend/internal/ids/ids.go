// Package ids generates the identifiers used for every row the platform creates.
package ids

import "github.com/google/uuid"

// New returns a fresh UUIDv7. uuid.NewV7 fails only if the system CSPRNG does, at which point
// nothing else the process does is trustworthy either, so this panics rather than threading an
// impossible error through every caller.
func New() uuid.UUID {
	return uuid.Must(uuid.NewV7())
}
