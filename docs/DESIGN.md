# Design

## The physics being emulated

A rubber bridge terminates the string on a soft, lossy support. Compared with
a hard bridge it:

1. adds damping to every partial, more at high frequencies;
2. therefore shortens sustain to a fraction of a second, with the top end
   gone almost immediately and the fundamental lingering as a "thunk";
3. leaves the pluck itself (the excitation) largely intact.

For an exponentially decaying partial, decay **rates add**:

```
rate_total(f) = rate_natural(f) + rate_bridge(f)
rate_bridge(f) = R · (f / 1 kHz)^α          [nepers per second]
```

So the bridge can be applied to an ordinary guitar signal as an *extra*
exponential decay per frequency that **restarts on every pluck**. The output
keeps the instrument's own character (its natural decay, body and pickup)
multiplied by the bridge's extra loss.

Parameter mapping:

| | |
|---|---|
| `R = 100 · Mute^1.5` | Mute 100 % → 100 Np/s at 1 kHz (T60 ≈ 69 ms); default 55 % → 41 Np/s (T60 ≈ 170 ms at 1 kHz, ≈ 0.9 s at 100 Hz) |
| `α = 0.2 + Damping` | 0.2 (nearly flat) to 1.2 (highs die much faster) |

## Signal flow (all IIR, zero latency)

```
in ──► 8-band Linkwitz-Riley tree (4th order, all-pass compensated)
          │                    │
          │            band envelopes ──► pluck detector ──► re-open gains
          ▼                                                   │
       Σ band_k · g_k(t)  ◄──── per-band gain g_k: decays at rate_bridge(f_k)
          │
         out
```

### Bands

Crossovers per instrument (Hz):

| Instrument | crossovers | lowest note (detector timing) |
|---|---|---|
| Electric | 125, 220, 400, 720, 1300, 2300, 4200 | 70 Hz (drop D) |
| Acoustic | 135, 240, 430, 780, 1400, 2500, 4500 | 70 Hz |
| Baritone | 90, 160, 290, 520, 950, 1700, 3100 | 44 Hz (A1/F#1 tunings) |

Eight bands at roughly 0.85-octave spacing keep the per-band decay rate within
about ±25 % of the continuous law. With the LR4 tree, the bands sum to an
all-pass, so 0 % Mute or 0 % Mix is magnitude-flat (tested within 0.1 dB, 40 Hz
to 12 kHz).

**Trade-off:** the all-pass delays the lows by a few ms relative to the highs.
That alone lowers a pluck's 5 ms peak by about 2–2.6 dB (same energy). A
complementary "difference of low-passes" structure avoids this, and was tried:
its 12 dB/oct skirts let slower-decaying neighbouring bands leak through, which
broke the per-harmonic decay accuracy. Decay accuracy is the point of the
plugin, so the LR4 tree stayed.

### Gain computer

Each band gain `g_k` is multiplied by `exp(−rate_bridge(f_k)/fs)` every sample
while the band has signal. In real silence (every band below −90 dBFS) it
drifts back to 1, ready for the next note.

### Pluck detection (the hard part)

The plugin only sees the mixed guitar signal, so it must decide when a string
was plucked again:

- **Peak-hold flux.** Per band, a peak follower is compared with the highest
  peak of the previous 2.4 periods of the instrument's lowest note. A ringing
  note never exceeds its own recent peaks (that also rejects the spiky,
  periodic envelope of a bright low string). A new pluck does. Rises are
  summed over the bands above 250 Hz.
- **Threshold.** The default 4.75 dB (Sensitivity 50 %) sits between:
  - sustained-chord beating, which peaks at 4.55 dB summed on the test set;
  - the softest re-pick (a soft note right after an accent on the same
    string), at 5–10 dB.

  The margin is narrow, which is why Sensitivity is exposed.
- **Re-opening.** A detected pluck pushes each band back open within ~2
  samples, then holds it for 2 ms (up to 30 ms with Attack > 50 %). Bands
  that rose themselves always re-open fully. The others re-open according to
  **Retrigger**, which only affects bands below ~400 Hz.

**Why Retrigger exists.** In the low bands, "melody note over a ringing bass"
and "re-picked bass" look identical: the bass band's level doesn't change in
either case. No level-based detector can tell them apart. High Retrigger
favours riffs and strumming (re-picks come back at full level). Low Retrigger
favours fingerstyle (the bass stays muted under the melody).

Approaches tried and rejected:

- *Per-band "admit new energy" on any envelope rise:* string beating looked
  like new notes, so ringing chords re-opened.
- *"String-stop dip" detector* (the pick touching the string causes a dip
  before a re-pick): the periodic dips in a ringing note's upper bands
  triggered it constantly.

## Verification without recordings

`tools/GuitarSynth.h` is a modal plucked-string synthesiser. It has
inharmonicity, two polarisations (beating), pluck and pickup comb filtering,
in-phase mode starts, pick clicks, string stops on re-picks, and acoustic body
modes. It renders every scenario twice:

- **dry:** ordinary bridge;
- **ref:** the same performance with the bridge law applied *per partial*.
  This is the ideal output of a perfect rubber-bridge processor.

`rb_tests` (895 checks) and `rb_eval` compare the plugin's output against `ref`:

| Test | What it checks |
|---|---|
| Transparency | Mute 0 % or Mix 0 % is flat within 0.1 dB, at 44.1/48/96 kHz, all instruments |
| Per-harmonic decay | 540 harmonic decays: decay rate within −35 %/+50 % of the physics (electric and baritone, 3 Mute × 2 Damping settings) |
| Sustained chord | No band gain ever re-opens while a chord rings |
| Attack | Every note's 5 ms attack peak within 3.5 dB of `ref` (including the ~2.5 dB all-pass effect) |
| Re-strum / riff | Every stroke within −3.5/+4 dB of `ref` |
| Melody over bass | With Retrigger 0 the bass stays muted (within 10 dB of `ref`, ≥ 15 dB below dry) |
| Overall | Mean per-band envelope error < 35 % of dry's (all scenarios) |
| Robustness | Silence in gives silence out; loud noise stays finite; output identical for any block size; stereo linked; every parameter randomised every 32 samples |

Current result: overall mean envelope error vs. physics is **4.8 dB**, against
**24.4 dB** for the unprocessed guitar.

Known limits, all visible in the numbers above:

- Within one band all partials share a gain, so two strings beating in the
  same band can beat a little deeper than in the physics.
- The very top harmonics of a note (above ~4 kHz) decay about 25–40 % slower
  than the law at extreme settings. They are gone within ~40 ms either way.
- Upstrokes that miss the low strings re-open those strings too (+3 dB vs
  `ref`). Lower Retrigger trades this against riff punch.

## Calibrating with real recordings

The synthetic guitar fixes the *method*. Real recordings will fix the
*numbers*: the decay law (R, α), the detector threshold and the defaults.
Useful takes, each with and without the mute (same guitar, pick, position and
gain; 24-bit, 48 kHz, no processing; DI for electric, mic for acoustic, plus
piezo if fitted):

1. Each open string, plus the 5th and 12th frets, at soft, medium and hard
   dynamics. Let each ring out fully.
2. A few strummed chords, let ring.
3. Accented riffs on one string (accent, then soft 8ths and 16ths): the
   hardest case for the detector.
4. Fingerstyle: a melody over a ringing bass note.
5. The same on a baritone, if available.

From (1), `rate_bridge(f)` can be measured per harmonic exactly as
`testDecayMatchesPhysics` does, and R and α fitted. From (2)–(4) on the
**unmuted** takes, the detector's flux distributions (onsets vs. sustain) give
the real separating threshold.

## Sources, and what is assumed

**Taken from the literature:**

- *Decay rates add; T60 = ln(1000) / rate.* Standard damped-oscillator
  physics. Each loss mechanism (air, internal friction, energy leaving through
  the bridge) contributes its own damping term, and they sum. See Fletcher &
  Rossing, *The Physics of Musical Instruments* (2nd ed., Springer, 1998),
  ch. 2 and 9, and J. O. Smith, *Physical Audio Signal Processing* (online
  book, CCRMA), on string loss and T60.
- *String losses grow with frequency, and the bridge/support is a major loss
  path.* Valette & Cuesta, *Mécanique de la corde vibrante* (Hermès, 1993);
  Gough, "The theory of string resonances on musical instruments", *Acustica*
  48 (1981); Woodhouse, "On the synthesis of guitar plucks", *Acta Acustica
  united with Acustica* 90 (2004).
- *Frequency-dependent loss per round trip as the core of plucked-string
  models.* Jaffe & Smith, "Extensions of the Karplus-Strong plucked-string
  algorithm", *Computer Music Journal* 7(2) (1983); Karjalainen, Välimäki &
  Tolonen, "Plucked-string models: from the Karplus-Strong algorithm to
  digital waveguides and beyond", *Computer Music Journal* 22(3) (1998).
- *Stiff-string inharmonicity* `f_n = n·f0·sqrt(1 + B·n²)` (used in the test
  synth): Fletcher, "Normal vibration frequencies of a stiff piano string",
  *JASA* 36 (1964).
- *Plucked-string mode amplitudes* `∝ sin(nπ·p)/n^k` for pluck position `p`,
  and pickup comb filtering `sin(nπ·q)`: Fletcher & Rossing, ch. 2.
- *Linkwitz-Riley crossovers* (LR4 sums to an all-pass): Linkwitz, "Active
  crossover networks for noncoincident drivers", *JAES* 24(1) (1976).
- *TPT state-variable filter*: Zavalishin, *The Art of VA Filter Design*
  (Native Instruments, 2012/2018); Simper, "Linear trapezoidal integrated SVF"
  (Cytomic technical paper).
- *Onset detection by summed positive band-energy rises ("spectral flux")*:
  Bello et al., "A tutorial on onset detection in music signals", *IEEE Trans.
  Speech and Audio Processing* 13(5) (2005). The peak-hold-over-a-period
  variant here is my adaptation for zero latency.

**My modelling choices (not from a paper, to be calibrated with recordings):**

- The *form* of the bridge loss, `R·(f/1kHz)^α`. It is a simple power law
  that captures "more loss at higher frequencies". I know of no published
  measurement of rubber-bridge damping to fit.
- The *numbers*: R up to 100 Np/s at 1 kHz and α from 0.2 to 1.2 were picked
  so the extremes sound like "barely muted" and "heavily muted".
- The synthetic guitar's natural decay, `T60(f) = 7 s / (1 + f/400)^0.8`, is
  a plausible approximation of a steel string (several seconds at the low
  end, under a second above 5 kHz). It is not fitted to measurements.
- All detector constants (thresholds, windows, dead zones) were tuned on the
  synthetic scenarios.

Recordings with and without a real mute (see above) replace these
assumptions with measurements.
