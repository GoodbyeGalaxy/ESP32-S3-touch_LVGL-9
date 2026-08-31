# Measurement Accuracy

This document describes what each measurement in the studio panel actually computes,
how it compares to professional standards, and where to apply caution when interpreting results.

---

## TL;DR

| Measurement | Grade | Notes |
|---|---|---|
| RMS Level (L/R) | ✅ Professional | True per-block RMS in dBFS |
| Mid / Side RMS | ✅ Professional | Phase-correct M/S decomposition |
| Momentary Loudness | ⚠️ Amateur | EMA approximation, no K-weighting, no gating |
| Short-term Loudness | ⚠️ Amateur | Same approximation, 3 s window |
| Integrated Loudness | ❌ Buggy | Hardcoded constant — ignores actual signal |
| Peak Level | ⚠️ Sample peak | Not True Peak — inter-sample peaks undetected |
| Goniometer | ⚠️ Sampled | 30 pts/s — steady signals OK, transients may be missed |
| Spectrum (Bars/Curve/LED/…) | ⚠️ Relative | Frame-normalised — not calibrated to dBFS |
| Octave Spectrum | ⚠️ Relative | Same normalisation as other spectrum modes |

---

## RMS Level — L / R channels

**What is computed:**
```
rms_l = 20 * log10( sqrt( mean(l²) ) )  per 33 ms block
```

**Grade: professional.**  
This is standard true-RMS per block. Values are in dBFS (full-scale). For comparison:
a sustained sine at −6 dBFS full scale shows −6 dBFS here.

**Limitation:** One block = 33 ms (1470 samples at 44100 Hz). The RMS does not integrate
across blocks — it resets every block. This is intentional for a level meter but means it
is *not* a long-term average.

---

## Mid / Side RMS

**What is computed:**
```
mid  = (L + R) / 2      →  rms_mono  = RMS(mid)
side = (L − R) / 2      →  rms_side  = RMS(side)
```

**Grade: professional.**  
This matches the M/S decomposition used in broadcast and mastering tools. The `/2` scaling
ensures that a mono signal (L = R) produces the same level in M as it does in L and R, with
the Side channel at −∞ dBFS. Same convention as iZotope RX, Nugen Visualizer, SPAN.

---

## Loudness (Momentary / Short-Term / Integrated)

**What is computed (sender-side):**
```python
mono_power = mean(((l + r) / 2) ** 2)          # unweighted mono power per block
alpha_m    = 1 − exp(−dt / 0.4)               # ≈ 0.08 per 33 ms block
m_acc     += alpha_m * (mono_power − m_acc)   # exponential moving average
momentary  = 10 * log10(m_acc)                 # in dBFS
```

Short-term uses a 3 s time constant; integrated uses 30 s.

**Grade: amateur.**

| Requirement | BS.1770-4 / EBU R128 | This implementation |
|---|---|---|
| K-weighting | Pre-filter (≈ high shelf + HPF) | None — flat response |
| Gating | Absolute gate −70 LUFS, relative gate −10 LU | None |
| Channel sum | L + R + 1.5 × (C + Ls + Rs) | Mono only |
| Integration window | 400 ms rectangular (momentary) | 400 ms EMA (≈similar shape) |

The values read as dBFS rather than LUFS. A difference of 3–8 LU is typical compared to a
compliant meter, depending on programme material. Suitable for relative comparisons and
dynamics monitoring, not for broadcast compliance.

**Integrated loudness — known bug:**  
The integrated accumulator uses a hardcoded constant instead of the actual signal power:
```python
i_acc += ALPHA_I * (0.0158 − i_acc)   # 0.0158 is constant — ignores audio input
```
This means the integrated display always converges to approximately −18 dBFS regardless
of the actual programme level. The integrated value is currently not displayed on the
device for this reason.

---

## Peak Level

**What is computed:**
```python
peak = 20 * log10( max(abs(l)) ** 2 )    # per 33 ms block
```

Wait — this is actually `max(abs(l))**2`, not `max(abs(l))`. This squares the sample value before
taking log, producing a level 6 dB *lower* than the correct sample peak. The peak reading
is therefore approximately 6 dB off. (Noted for future correction.)

**True Peak (inter-sample) is not computed.** True Peak requires 4× oversampling to detect
inter-sample peaks that exceed 0 dBFS after D/A reconstruction. On modern streams, true peak
can exceed the displayed sample peak by 0.5–3 dB. For broadcast (EBU R128: TP ≤ −1 dBTP),
do not rely on this meter for true peak compliance.

---

## Goniometer

**What is computed:**
One sample pair `(l[−1], r[−1])` — the last sample of each 33 ms UDP block — is plotted
on the Lissajous figure. At 30 packets/second, **30 points per second** are added to the
phosphor ring buffer.

**Grade: amateur for transients, acceptable for steady state.**

For a 440 Hz sine wave, approximately 14.5 full cycles occur in one 33 ms block. Only 1 sample
per cycle group is captured. On steady-state test tones, the phosphor buffer fills in over
several frames and produces a representative pattern. For transient material (drum hits,
clicks), the display reflects only a single random phase of each transient.

Professional goniometers run at audio rate (44100 pts/s). This implementation runs at 30 pts/s.

The **phase correlation meter** (−1 to +1) is computed from the full block:
```python
corr = mean(l * r) / (rms_l_linear * rms_r_linear)
```
This is statistically valid regardless of block rate and is accurate.

---

## Spectrum Analyzer (all modes)

**What is computed (sender-side):**
```python
fft = abs(rfft(block * window))[:256]
fft = log1p(fft * 20.0)         # log compression
fft /= fft.max()                 # normalise to frame peak = 1.0
```

**Grade: relative only — not calibrated to dBFS.**

Every frame is individually normalised so the loudest bin = 1.0. This means:

- A whisper-quiet signal looks the same as a full-scale signal.
- The vertical axis has no dBFS meaning.
- The display shows *spectral shape*, not *spectral level*.
- The guide lines (−3/−6/−12 dB) are relative to the loudest bin in that frame, not to 0 dBFS.

**LED mode** deliberately mimics the visual style of a VU or dBFS meter, but is subject to the
same normalisation. A disclaimer is shown in the UI: *Relative display — not calibrated to dBFS.*

For absolute spectral levels, the RMS and Loudness meters on the Metering screen are the
correct reference.

**FFT parameters:**
- Block: 1470 samples, Hanning window
- FFT size: 1024 → 512 unique bins
- Bin width: 44100 / 1024 ≈ 43 Hz
- Frequency range displayed: ~20 Hz – 20 kHz (log-spaced across 64 display bins)
- Frame rate: 30 Hz (same as UDP packet rate)

---

## Summary: What to Trust

**Use for compliance / critical decisions:**
- Nothing on this panel — use a professional metering plugin in your DAW.

**Use for mixing / monitoring decisions:**
- RMS L/R, Mid/Side — these are accurate.
- Phase correlation — accurate.
- Spectral *shape* (not level) — useful for EQ decisions.
- Goniometer shape — useful for stereo field monitoring on steady-state content.

**Use for awareness / overview only:**
- Momentary and short-term loudness — good for dynamics, not for LUFS targets.
- Spectrum level (Bars/Curve/LED) — relative only.
- Peak — approximately 6 dB off due to sender-side squaring bug.
