package scenario

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

func decode(t *testing.T, s string) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal([]byte(s), &m); err != nil {
		t.Fatal(err)
	}
	return m
}

func TestParsePointerFollowsRFC6901Escaping(t *testing.T) {
	cases := map[string]Pointer{
		"/a/b":  {"a", "b"},
		"/a~1b": {"a/b"},
		"/m~0n": {"m~n"},
		"/~01":  {"~1"}, // ~0 decodes first-in-order-of-appearance, never producing "/"
		"/":     {""},
		"/0/x":  {"0", "x"},
	}
	for in, want := range cases {
		got, err := ParsePointer(in)
		if err != nil || !reflect.DeepEqual(got, want) {
			t.Errorf("ParsePointer(%q) = %v, %v; want %v", in, got, err, want)
		}
		if got.String() != in {
			t.Errorf("round trip of %q gave %q", in, got.String())
		}
	}
	for _, bad := range []string{"", "a/b", "/a~2", "/~"} {
		if _, err := ParsePointer(bad); err == nil {
			t.Errorf("ParsePointer(%q) accepted", bad)
		}
	}
}

func TestOverrideReplacesExistingValues(t *testing.T) {
	spec := decode(t, `{"materials":[{"viscosity":5.0,"name":"water"}],"numerics":{"h":0.025}}`)
	if err := Override(spec, "/materials/0/viscosity", 1.5); err != nil {
		t.Fatal(err)
	}
	if err := Override(spec, "/numerics/h", 0.02); err != nil {
		t.Fatal(err)
	}
	if spec["materials"].([]any)[0].(map[string]any)["viscosity"] != 1.5 ||
		spec["numerics"].(map[string]any)["h"] != 0.02 {
		t.Fatalf("overrides not applied: %v", spec)
	}
}

func TestOverrideRejectsTyposAndTypeChanges(t *testing.T) {
	cases := []struct {
		ptr   string
		value any
		want  string
	}{
		{"/numerics/xsph", 0.5, "does not exist"},       // typo: the solver would silently ignore it
		{"/numerics/h", "0.02", "type mismatch"},        // string for a number
		{"/materials/1/viscosity", 1.0, "out of range"}, // no second material
		{"/materials/00/viscosity", 1.0, "invalid array index"},
		{"/materials/-/viscosity", 1.0, "invalid array index"},
		{"/numerics/h/deeper", 1.0, "not an object or array"},
	}
	for _, c := range cases {
		spec := decode(t, `{"materials":[{"viscosity":5.0}],"numerics":{"h":0.025}}`)
		err := Override(spec, c.ptr, c.value)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("Override(%s, %v) = %v; want error containing %q", c.ptr, c.value, err, c.want)
		}
	}
}

func TestOverrideDoesNotAliasTheValue(t *testing.T) {
	spec := decode(t, `{"v":[1,2,3]}`)
	val := []any{4.0, 5.0, 6.0}
	Override(spec, "/v", val)
	val[0] = 99.0
	if spec["v"].([]any)[0] != 4.0 {
		t.Fatal("override aliases the caller's value")
	}
}

func TestApplyOverridesReportsEveryErrorInSortedOrder(t *testing.T) {
	spec := decode(t, `{"a":1,"b":2}`)
	errs := ApplyOverrides(spec, map[string]any{"/z": 1.0, "/b": "x", "/a": 3.0}, "overrides")
	if len(errs) != 2 || errs[0].Field != "overrides[/b]" || errs[1].Field != "overrides[/z]" {
		t.Fatalf("got %+v", errs)
	}
	if spec["a"] != 3.0 {
		t.Fatal("valid override not applied alongside invalid ones")
	}
}

// The cache is keyed on this hash, so two requests that mean the same
// thing must hash the same, and any difference that changes results must
// change the hash.
func TestSpecHashIsCanonical(t *testing.T) {
	a := decode(t, `{"x": 1.0, "y": {"b": [1, 2], "a": "s"}}`)
	b := decode(t, `{"y":{"a":"s","b":[1.0,2.00]},"x":1}`)
	p := RunParams{Quality: "low", MaxParticles: 1000}
	ha, _ := SpecHash(a, p)
	hb, _ := SpecHash(b, p)
	if ha != hb {
		t.Fatal("semantically equal specs hash differently")
	}
	t1 := 0.5
	for name, q := range map[string]RunParams{
		"quality":       {Quality: "medium", MaxParticles: 1000},
		"sim_time":      {Quality: "low", MaxParticles: 1000, SimTime: &t1},
		"max_particles": {Quality: "low", MaxParticles: 999},
	} {
		if h, _ := SpecHash(a, q); h == ha {
			t.Errorf("changing %s did not change the hash", name)
		}
	}
	c := decode(t, `{"x": 1.0000001, "y": {"b": [1, 2], "a": "s"}}`)
	if hc, _ := SpecHash(c, p); hc == ha {
		t.Error("a changed value did not change the hash")
	}
}

func TestCacheKeyDependsOnSolver(t *testing.T) {
	if CacheKey("spec", "solverA") == CacheKey("spec", "solverB") {
		t.Fatal("different solver binaries share a cache key")
	}
}

func TestExpandGridIsCartesianAndDeterministic(t *testing.T) {
	grid := map[string][]any{"/b": {1.0, 2.0}, "/a": {"x", "y", "z"}}
	pts, err := ExpandGrid(grid, 100)
	if err != nil || len(pts) != 6 {
		t.Fatalf("%v %v", pts, err)
	}
	// Axes sorted (/a before /b); last axis fastest.
	want := []GridPoint{
		{"/a": "x", "/b": 1.0}, {"/a": "x", "/b": 2.0},
		{"/a": "y", "/b": 1.0}, {"/a": "y", "/b": 2.0},
		{"/a": "z", "/b": 1.0}, {"/a": "z", "/b": 2.0},
	}
	if !reflect.DeepEqual(pts, want) {
		t.Fatalf("got %v", pts)
	}
}

func TestExpandGridRejectsOversizeWithoutAllocating(t *testing.T) {
	huge := map[string][]any{}
	vals := make([]any, 1000)
	for i := 0; i < 8; i++ { // 1000^8 would overflow int64 if multiplied naively
		huge[string(rune('a'+i))] = vals
	}
	if _, err := ExpandGrid(huge, 256); err == nil {
		t.Fatal("accepted a grid of 10^24 points")
	}
	if _, err := ExpandGrid(map[string][]any{"/a": {}}, 10); err == nil {
		t.Fatal("accepted an empty axis")
	}
}

func TestCatalogLoadsTheRealScenarioLibrary(t *testing.T) {
	c, err := LoadCatalog("../../../configs/scenarios")
	if err != nil {
		t.Fatal(err)
	}
	if len(c.List()) != 14 {
		t.Fatalf("loaded %d scenarios, want 14", len(c.List()))
	}
	e, ok := c.Get("dam_break")
	if !ok || e.Tier != 1 || e.Description == "" {
		t.Fatalf("dam_break: %+v", e)
	}
	a, _ := c.Base("dam_break")
	Override(a, "/numerics/h", 0.5)
	b, _ := c.Base("dam_break")
	if b["numerics"].(map[string]any)["h"] == 0.5 {
		t.Fatal("Base returned an alias of the catalogue entry")
	}
}
