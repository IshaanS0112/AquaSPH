// Package ids generates the identifiers used for every row the platform
// creates.
//
// UUIDv7 rather than v4: the leading 48 bits are a millisecond timestamp,
// so IDs sort by creation time. That makes the primary key double as the
// pagination key (TRD §5) and keeps B-tree inserts append-mostly instead
// of scattering across the index the way random v4 IDs do.
package ids

import "github.com/google/uuid"

// New returns a fresh UUIDv7. uuid.NewV7 fails only if the system CSPRNG
// does, at which point nothing else the process does is trustworthy
// either, so this panics rather than threading an impossible error
// through every caller.
func New() uuid.UUID {
	return uuid.Must(uuid.NewV7())
}
