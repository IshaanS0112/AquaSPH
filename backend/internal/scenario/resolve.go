package scenario

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"sort"
)

// kind names the JSON type of a decoded value.
func kind(v any) string {
	switch v.(type) {
	case nil:
		return "null"
	case bool:
		return "boolean"
	case float64, json.Number:
		return "number"
	case string:
		return "string"
	case []any:
		return "array"
	case map[string]any:
		return "object"
	default:
		return fmt.Sprintf("%T", v)
	}
}

// FieldError is one problem with one part of a request.
type FieldError struct {
	Field   string `json:"field"`
	Message string `json:"message"`
}

// Override applies one pointer override to spec.
//
// Two rules, both there to catch mistakes at submit time instead of after
// minutes of compute:
//
//  1. The target must already exist. The solver ignores unknown keys, so
//     "/numerics/xsph" (a typo for xsph_epsilon) would otherwise be
//     accepted, ignored, and cost a full run that varies nothing.
//  2. The new value must have the same JSON type as the old one, so "0.5"
//     cannot replace 0.5.
//
// Setting an optional field the base scenario omits requires an inline spec.
func Override(spec map[string]any, pointer string, value any) error {
	p, err := ParsePointer(pointer)
	if err != nil {
		return err
	}
	old, err := p.Get(spec)
	if err != nil {
		return err
	}
	if old != nil && kind(old) != kind(value) {
		return fmt.Errorf("type mismatch: the scenario has a %s here, got a %s", kind(old), kind(value))
	}
	return p.Replace(spec, DeepCopy(value))
}

// ApplyOverrides applies overrides in sorted pointer order, so the error
// reported for a request with several bad overrides is deterministic.
func ApplyOverrides(spec map[string]any, overrides map[string]any, fieldPrefix string) []FieldError {
	keys := make([]string, 0, len(overrides))
	for k := range overrides {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var errs []FieldError
	for _, k := range keys {
		if err := Override(spec, k, overrides[k]); err != nil {
			errs = append(errs, FieldError{Field: fieldPrefix + "[" + k + "]", Message: err.Error()})
		}
	}
	return errs
}

// DeepCopy copies a decoded JSON value so overrides applied to one job's
// spec can never alias the catalogue or another job.
func DeepCopy(v any) any {
	switch t := v.(type) {
	case map[string]any:
		out := make(map[string]any, len(t))
		for k, val := range t {
			out[k] = DeepCopy(val)
		}
		return out
	case []any:
		out := make([]any, len(t))
		for i, val := range t {
			out[i] = DeepCopy(val)
		}
		return out
	default:
		return t
	}
}

// Canonical returns the canonical JSON encoding of v: encoding/json sorts
// map keys and formats each float64 in its shortest round-trip form, so two
// semantically equal documents (1.0 vs 1, different key order, different
// whitespace) encode to the same bytes.
func Canonical(v any) ([]byte, error) {
	return json.Marshal(v)
}

// RunParams are the run settings that change results, and therefore
// belong in the cache key. Threads are deliberately absent: the solver is
// bit-identical across thread counts (ADR-0002).
type RunParams struct {
	Quality      string   `json:"quality"`
	SimTime      *float64 `json:"sim_time"`
	MaxSteps     *int     `json:"max_steps"`
	MaxParticles int      `json:"max_particles"`
}

// SpecHash identifies what will be computed, independent of the solver.
func SpecHash(spec map[string]any, p RunParams) (string, error) {
	b, err := Canonical(map[string]any{"spec": spec, "run": p})
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:]), nil
}

// CacheKey identifies a result: what was computed, and by which solver
// binary.
func CacheKey(specHash, solverID string) string {
	sum := sha256.Sum256([]byte(specHash + ":" + solverID))
	return hex.EncodeToString(sum[:])
}
