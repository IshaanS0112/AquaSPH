package artifacts

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"io"
	"os"
	"strings"
	"testing"

	"github.com/google/uuid"
)

func TestPutThenOpenRoundTripsWithHash(t *testing.T) {
	fs, _ := NewLocalFS(t.TempDir())
	job := uuid.New()
	a, err := fs.Put(context.Background(), job, "metrics.json", strings.NewReader(`{"ok":true}`))
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256([]byte(`{"ok":true}`))
	if a.Size != 11 || a.SHA256 != hex.EncodeToString(sum[:]) || a.ContentType != "application/json" {
		t.Fatalf("%+v", a)
	}
	f, err := fs.Open(context.Background(), job, "metrics.json")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	b, _ := io.ReadAll(f)
	if string(b) != `{"ok":true}` {
		t.Fatalf("read %q", b)
	}
	entries, _ := os.ReadDir(fs.root + "/" + job.String())
	if len(entries) != 1 {
		t.Fatalf("temporary files left behind: %v", entries)
	}
}

func TestNamesThatCouldEscapeTheJobDirectoryAreRejected(t *testing.T) {
	fs, _ := NewLocalFS(t.TempDir())
	for _, name := range []string{"../x", "..", "a/b", "/etc/passwd", "", ".hidden", "a..b.json",
		"UPPER.json", "a\x00b", strings.Repeat("a", 100)} {
		if _, err := fs.Put(context.Background(), uuid.New(), name, strings.NewReader("x")); err == nil {
			t.Errorf("Put accepted %q", name)
		}
		if _, err := fs.Open(context.Background(), uuid.New(), name); !errors.Is(err, ErrNotFound) {
			t.Errorf("Open(%q) = %v, want ErrNotFound", name, err)
		}
	}
	for _, name := range []string{"metrics.json", "progress.jsonl", "stderr.log", "frame_0001.png", "scenario.json"} {
		if !ValidName(name) {
			t.Errorf("rejected legitimate name %q", name)
		}
	}
}

func TestOpenMissingIsNotFound(t *testing.T) {
	fs, _ := NewLocalFS(t.TempDir())
	if _, err := fs.Open(context.Background(), uuid.New(), "metrics.json"); !errors.Is(err, ErrNotFound) {
		t.Fatal(err)
	}
}
