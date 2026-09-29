package scenario

import (
	"fmt"
	"sort"
)

// GridPoint is one combination of sweep parameters: pointer -> value.
type GridPoint map[string]any

// ExpandGrid returns the cartesian product of the axes in a deterministic order: axes sorted by
// pointer, then values in the order given, with the last axis varying fastest.
func ExpandGrid(grid map[string][]any, max int) ([]GridPoint, error) {
	if len(grid) == 0 {
		return nil, fmt.Errorf("grid must have at least one axis")
	}
	axes := make([]string, 0, len(grid))
	total := 1
	for p, values := range grid {
		if len(values) == 0 {
			return nil, fmt.Errorf("axis %s has no values", p)
		}
		axes = append(axes, p)
		if total > max/len(values) {
			return nil, fmt.Errorf("grid has more than %d combinations", max)
		}
		total *= len(values)
	}
	if total > max {
		return nil, fmt.Errorf("grid has %d combinations, more than the limit of %d", total, max)
	}
	sort.Strings(axes)

	out := make([]GridPoint, 0, total)
	idx := make([]int, len(axes))
	for {
		pt := make(GridPoint, len(axes))
		for i, a := range axes {
			pt[a] = grid[a][idx[i]]
		}
		out = append(out, pt)
		k := len(axes) - 1
		for k >= 0 {
			idx[k]++
			if idx[k] < len(grid[axes[k]]) {
				break
			}
			idx[k] = 0
			k--
		}
		if k < 0 {
			return out, nil
		}
	}
}
