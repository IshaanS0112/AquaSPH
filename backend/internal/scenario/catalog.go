package scenario

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

// Entry is one scenario in the catalogue.
type Entry struct {
	Name          string         `json:"name"`
	Tier          int            `json:"tier"`
	Description   string         `json:"description"`
	Approximation string         `json:"approximation,omitempty"`
	Spec          map[string]any `json:"-"`
}

// Catalog is the read-only set of named scenarios, loaded once at start-up
// from the same configs/scenarios directory the CLI uses.
type Catalog struct {
	entries map[string]*Entry
	names   []string
}

var nameRE = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,63}$`)

// ValidName reports whether s is an acceptable scenario name.
func ValidName(s string) bool { return nameRE.MatchString(s) }

// LoadCatalog reads every *.json file in dir.
func LoadCatalog(dir string) (*Catalog, error) {
	files, err := filepath.Glob(filepath.Join(dir, "*.json"))
	if err != nil {
		return nil, err
	}
	if len(files) == 0 {
		return nil, fmt.Errorf("no scenarios found in %s", dir)
	}
	c := &Catalog{entries: map[string]*Entry{}}
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			return nil, err
		}
		var spec map[string]any
		if err := json.Unmarshal(b, &spec); err != nil {
			return nil, fmt.Errorf("%s: %w", f, err)
		}
		name := strings.TrimSuffix(filepath.Base(f), ".json")
		if !ValidName(name) {
			return nil, fmt.Errorf("%s: invalid scenario name", f)
		}
		e := &Entry{Name: name, Spec: spec}
		if t, ok := spec["tier"].(float64); ok {
			e.Tier = int(t)
		}
		e.Description, _ = spec["description"].(string)
		e.Approximation, _ = spec["approximation"].(string)
		c.entries[name] = e
		c.names = append(c.names, name)
	}
	sort.Strings(c.names)
	return c, nil
}

func (c *Catalog) Get(name string) (*Entry, bool) {
	e, ok := c.entries[name]
	return e, ok
}

func (c *Catalog) List() []*Entry {
	out := make([]*Entry, len(c.names))
	for i, n := range c.names {
		out[i] = c.entries[n]
	}
	return out
}

// Base returns a deep copy of the named scenario's spec, safe to modify.
func (c *Catalog) Base(name string) (map[string]any, bool) {
	e, ok := c.entries[name]
	if !ok {
		return nil, false
	}
	return DeepCopy(e.Spec).(map[string]any), true
}
