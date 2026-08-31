# Goniometer Display Modes

## Feature Overview

The goniometer visualises the phase relationship between the left and right channels as a Lissajous figure with phosphor-style persistence. Two display modes are available, switchable per screen via the settings overlay.

### XY — Oscilloscope

The left channel drives the horizontal axis; the right channel drives the vertical axis.

| Signal condition | Appearance |
|---|---|
| Mono (L = R) | Diagonal line at +45° |
| Fully cancelled (L = −R) | Diagonal line at −45° |
| Wide stereo | Ellipse tilted at 45° |
| Pure left | Horizontal line |
| Pure right | Vertical line |

**Reference guides:** green +45° diagonal marks the mono direction; red −45° diagonal marks the cancellation direction.

**Axis labels:** *L* (horizontal) · *R* (vertical)

### M/S — Industry Standard

The sum signal Mid = (L+R)/2 drives the vertical axis; the difference signal Side = (L−R)/2 drives the horizontal axis. This is the display convention used by professional metering tools (iZotope, Nugen, Waves).

| Signal condition | Appearance |
|---|---|
| Mono (L = R) | Vertical line |
| Fully cancelled (L = −R) | Horizontal line |
| Wide stereo | Wide horizontal spread |
| Pure left | Diagonal at +45° |
| Pure right | Diagonal at −45° |

**Reference guides:** green vertical marks the mono direction; red horizontal marks the cancellation direction.

**Axis labels:** *S* (horizontal) · *M* (vertical)

---

## Signal Accuracy

### Phase-Correct Mono Sum

The Mid (M) level meter reflects the true mono sum computed in the sample domain by the sender:

```
rms_mono = RMS( (L[n] + R[n]) / 2 )
```

This is computed per audio block from actual samples — not derived from the individual channel RMS values after the fact. The distinction matters:

- **Approximation (incorrect):** `mid_level ≈ (RMS_L + RMS_R) / 2`  
  Ignores phase. A 180°-inverted stereo pair reports the same mid level as a mono signal, masking complete cancellation.

- **Sample-domain (correct):** `mid_level = RMS((L+R)/2)`  
  Phase-destructive cancellation shows as near-silence, exactly as a club PA or broadcast mono sum would reproduce it.

The same principle applies to the Side level: `rms_side = RMS( (L[n] − R[n]) / 2 )`.

---

## Professional Positioning

- **Mono compatibility verification** — the Mid bar displays the level a club PA, streaming normalization stage, or broadcast mono fold would reproduce, not an approximation of it.
- **Quantified stereo width** — the Side bar measures energy outside the mono sum; a Side reading close to the Mid reading indicates a wide or decorrelated mix.
- **Phase issue detection** — a goniometer trace that collapses toward the cancellation axis (red guide line) combined with a collapsing Mid bar is unambiguous evidence of a phase problem that survives mono fold-down.
- **Industry-standard display** — the M/S mode matches the goniometer convention used by professional metering software, enabling direct comparison with a DAW's analyser without relearning axis orientation.
- **Dual evidence** — the STEREO screen combines goniometer (visual, qualitative), M/S bars (quantitative, dBFS), and phase correlation bar (scalar −1 to +1) into a single view, covering all evidence needed for a mono-compatibility sign-off.
