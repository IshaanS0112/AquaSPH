// Package artifacts stores the files a job produces.
package artifacts

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/IshaanS0112/AquaSPH/backend/internal/domain"
	"github.com/google/uuid"
)

var ErrNotFound = errors.New("artifact not found")

type Store interface {
	Put(ctx context.Context, jobID uuid.UUID, name string, r io.Reader) (domain.Artifact, error)
	Open(ctx context.Context, jobID uuid.UUID, name string) (*os.File, error)
}

// Names are a strict allowlist, checked on write *and* read: the download handler passes a name
// from the URL, and this is what makes "../../etc/passwd" impossible rather than merely
// unlikely.
var nameRE = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,62}(\.[a-z0-9]{1,8}){0,2}$`)

func ValidName(name string) bool {
	return nameRE.MatchString(name) && !strings.Contains(name, "..")
}

func ContentType(name string) string {
	switch filepath.Ext(name) {
	case ".json":
		return "application/json"
	case ".jsonl":
		return "application/x-ndjson"
	case ".log", ".txt":
		return "text/plain; charset=utf-8"
	case ".csv":
		return "text/csv; charset=utf-8"
	case ".png":
		return "image/png"
	default:
		return "application/octet-stream"
	}
}

type LocalFS struct {
	root string
}

func NewLocalFS(root string) (*LocalFS, error) {
	if err := os.MkdirAll(root, 0o750); err != nil {
		return nil, err
	}
	abs, err := filepath.Abs(root)
	if err != nil {
		return nil, err
	}
	return &LocalFS{root: abs}, nil
}

func (l *LocalFS) path(jobID uuid.UUID, name string) (string, error) {
	if !ValidName(name) {
		return "", fmt.Errorf("invalid artifact name %q", name)
	}
	return filepath.Join(l.root, jobID.String(), name), nil
}

// Put writes to a temporary file and renames it into place, so a reader never sees a
// half-written artifact and a crash mid-write leaves no corrupt file behind.
func (l *LocalFS) Put(_ context.Context, jobID uuid.UUID, name string, r io.Reader) (domain.Artifact, error) {
	dst, err := l.path(jobID, name)
	if err != nil {
		return domain.Artifact{}, err
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o750); err != nil {
		return domain.Artifact{}, err
	}
	tmp, err := os.CreateTemp(filepath.Dir(dst), "."+name+".tmp-*")
	if err != nil {
		return domain.Artifact{}, err
	}
	defer os.Remove(tmp.Name()) // no-op after a successful rename
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(tmp, h), r)
	if err == nil {
		err = tmp.Sync()
	}
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return domain.Artifact{}, err
	}
	if err := os.Rename(tmp.Name(), dst); err != nil {
		return domain.Artifact{}, err
	}
	return domain.Artifact{Name: name, Size: n, SHA256: hex.EncodeToString(h.Sum(nil)), ContentType: ContentType(name)}, nil
}

func (l *LocalFS) Open(_ context.Context, jobID uuid.UUID, name string) (*os.File, error) {
	p, err := l.path(jobID, name)
	if err != nil {
		return nil, ErrNotFound
	}
	f, err := os.Open(p)
	if errors.Is(err, os.ErrNotExist) {
		return nil, ErrNotFound
	}
	return f, err
}
