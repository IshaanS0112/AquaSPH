# Reference data for validation

`scripts/validate.py` compares AquaSPH's measured output against the
references in this directory.

## `martin_moyce_1952.json` — dam-break surge front

Martin, J. C. and Moyce, W. J. (1952), "An experimental study of the
collapse of liquid columns on a rigid horizontal plane", *Philosophical
Transactions of the Royal Society A*, 244(882), 312–324.

**This file ships with an empty `points` array, deliberately.** The
original tabulation was not available in the environment where this
project's validation was produced, and inventing plausible-looking
experimental numbers — or transcribing half-remembered ones from a
secondary source — would be worse than having none. A validation curve is
only worth anything if its reference is trustworthy.

The quantitative dam-break comparison that AquaSPH *does* ship is against
the Ritter (1892) analytical dry-bed solution, which is derived in
`docs/validation.md` rather than cited, so it can be checked line by line.

To add the experimental comparison, fill in `points` with
`[T, Z]` pairs from the paper and re-run:

```bash
python3 scripts/validate.py
```

`scripts/validate.py` picks the file up automatically and adds the
experimental series to the table and the plot. Nothing else needs
changing.

### Conventions the file must use

| Symbol | Meaning |
|---|---|
| `a` | initial column width (the horizontal dimension that collapses) |
| `h0` | initial column height |
| `Z` | surge-front position measured from the column's back wall, divided by `a` — so `Z = 1` at t = 0 |
| `T` | `t * sqrt(2 g / a)` |

AquaSPH's own metrics use exactly these definitions
(`MetricsCollector::sample` in `src/metrics/Metrics.cpp`), so a
correctly-transcribed table is directly comparable with no rescaling.
