// DSP tests: run with `ctest` or ./rb_tests. No framework, exits non-zero on failure.

#include "../dsp/RubberBridge.h"
#include "../tools/Analysis.h"
#include "../tools/GuitarSynth.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                    \
    do                                                      \
    {                                                       \
        ++checks;                                           \
        if (! (cond))                                       \
        {                                                   \
            ++failures;                                     \
            std::printf ("  FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf (__VA_ARGS__);                      \
            std::printf ("\n");                             \
        }                                                   \
    } while (0)

static std::vector<float> processMono (std::vector<float> x, double fs, const rb::Params& p, int block = 64)
{
    rb::RubberBridge dsp;
    dsp.prepare (fs, 1);
    dsp.setParams (p);
    dsp.reset();
    for (size_t pos = 0; pos < x.size(); pos += (size_t) block)
    {
        float* ptr = x.data() + pos;
        dsp.process (&ptr, 1, (int) std::min<size_t> ((size_t) block, x.size() - pos));
    }
    return x;
}

static rbsynth::Damping targetFor (const rb::Params& p)
{
    rbsynth::Damping d;
    d.ratePer1k = rb::RubberBridge::bridgeRateAt1k (p.mute);
    d.alpha = rb::RubberBridge::tiltExponent (p.damping);
    return d;
}

static const rbsynth::Scenario& scenario (const std::vector<rbsynth::Scenario>& all, const std::string& name)
{
    for (const auto& s : all)
        if (s.name == name)
            return s;
    std::printf ("missing scenario %s\n", name.c_str());
    std::exit (2);
}

static double rmsDb (const std::vector<float>& x, size_t a, size_t b)
{
    double e = 0.0;
    for (size_t i = a; i < b; ++i)
        e += (double) x[i] * x[i];
    return 10.0 * std::log10 (e / (double) (b - a) + 1.0e-20);
}

// ---------------------------------------------------------------------------

static void testTransparency()
{
    std::printf ("transparency (Mute 0, and Mix 0)\n");
    const double freqs[] = { 40, 80, 160, 300, 450, 800, 1300, 2500, 3600, 6000, 12000 };
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (int inst = 0; inst < 3; ++inst)
            for (int mode = 0; mode < 2; ++mode)
            {
                rb::Params p;
                p.instrument = (rb::Instrument) inst;
                p.thump = 0.0f;
                if (mode == 0)
                    p.mute = 0.0f;
                else
                {
                    p.mute = 1.0f;
                    p.mix = 0.0f;
                }
                for (double f : freqs)
                {
                    const size_t n = (size_t) (fs * 0.5);
                    std::vector<float> x (n);
                    for (size_t i = 0; i < n; ++i)
                        x[i] = (float) (0.5 * std::sin (2.0 * rb::kPi * f * (double) i / fs));
                    const auto y = processMono (x, fs, p);
                    const double d = rmsDb (y, n / 2, n) - rmsDb (x, n / 2, n);
                    CHECK (std::fabs (d) < 0.1, "fs=%g inst=%d mode=%d f=%g gain %.3f dB", fs, inst, mode, f, d);
                }
            }
}

static void testSilenceAndStability()
{
    std::printf ("silence & stability\n");
    rb::Params p;
    p.mute = 1.0f;
    p.thump = 1.0f;
    std::vector<float> zeros (48000, 0.0f);
    auto y = processMono (zeros, 48000, p);
    double mx = 0;
    for (float v : y)
        mx = std::max (mx, (double) std::fabs (v));
    CHECK (mx == 0.0, "silence produced output %g", mx);

    // Loud noise, full settings: output must stay finite and bounded.
    std::vector<float> noise (96000);
    uint32_t s = 1;
    for (auto& v : noise)
    {
        s = s * 1664525u + 1013904223u;
        v = (float) (((double) (s >> 8) / 16777216.0) * 2.0 - 1.0) * 4.0f;
    }
    y = processMono (noise, 48000, p);
    bool finite = true;
    mx = 0;
    for (float v : y)
    {
        finite = finite && std::isfinite (v);
        mx = std::max (mx, (double) std::fabs (v));
    }
    CHECK (finite, "non-finite output");
    CHECK (mx < 40.0, "output blew up: %g", mx);
}

static void testBlockSizeIndependence()
{
    std::printf ("block size independence\n");
    const auto all = rbsynth::makeScenarios (48000);
    const auto& sc = scenario (all, "electric_riff");
    const auto dry = rbsynth::render (sc.notes, sc.duration, sc.config, {});
    rb::Params p;
    const auto a = processMono (dry, 48000, p, 1);
    const auto b = processMono (dry, 48000, p, 512);
    double md = 0;
    for (size_t i = 0; i < a.size(); ++i)
        md = std::max (md, (double) std::fabs (a[i] - b[i]));
    CHECK (md < 1.0e-6, "block 1 vs 512 differ by %g", md);
}

static void testStereoLinked()
{
    std::printf ("stereo linked == mono\n");
    const auto all = rbsynth::makeScenarios (48000);
    const auto& sc = scenario (all, "acoustic_strum");
    const auto dry = rbsynth::render (sc.notes, sc.duration, sc.config, {});
    rb::Params p;
    p.instrument = rb::Instrument::Acoustic;
    const auto mono = processMono (dry, 48000, p);

    std::vector<float> l = dry, r = dry;
    rb::RubberBridge dsp;
    dsp.prepare (48000, 2);
    dsp.setParams (p);
    dsp.reset();
    for (size_t pos = 0; pos < l.size(); pos += 128)
    {
        float* ptrs[2] = { l.data() + pos, r.data() + pos };
        dsp.process (ptrs, 2, (int) std::min<size_t> (128, l.size() - pos));
    }
    double md = 0;
    for (size_t i = 0; i < l.size(); ++i)
        md = std::max ({ md, (double) std::fabs (l[i] - mono[i]), (double) std::fabs (r[i] - mono[i]) });
    CHECK (md < 1.0e-6, "stereo differs from mono by %g", md);
}

// Narrow band-pass (two cascaded SVF band-passes) around one harmonic.
static std::vector<float> bandpass (const std::vector<float>& x, double fs, double f, double q)
{
    rb::SvfCoeffs c;
    c.set (f, q, fs);
    rb::SvfState s1, s2;
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        double lp, bp, hp, bp2;
        s1.tick (c, x[i], lp, bp, hp);
        s2.tick (c, bp * c.k, lp, bp2, hp);
        y[i] = (float) (bp2 * c.k);
    }
    return y;
}

// Decay of individual harmonics must follow the physical target (ref).
static void testDecayMatchesPhysics()
{
    std::printf ("per-harmonic decay rates vs physical reference\n");
    const double fs = 48000;
    const auto all = rbsynth::makeScenarios (fs);
    int measured = 0;
    for (const char* name : { "electric_open_strings", "baritone_open_strings" })
        for (float mute : { 0.35f, 0.55f, 0.8f })
            for (float damping : { 0.2f, 0.8f })
            {
                const auto& sc = scenario (all, name);
                rb::Params p;
                p.mute = mute;
                p.damping = damping;
                p.thump = 0.0f;
                p.instrument = (rb::Instrument) sc.instrument;
                const auto dry = rbsynth::render (sc.notes, sc.duration, sc.config, {});
                const auto ref = rbsynth::render (sc.notes, sc.duration, sc.config, targetFor (p));
                const auto out = processMono (dry, fs, p);
                const double fr = 0.004;
                for (size_t ni = 0; ni < sc.notes.size(); ni += 1)
                {
                    const double t0 = sc.notes[ni].start;
                    const double t1 = ni + 1 < sc.notes.size() ? sc.notes[ni + 1].start : sc.duration;
                    for (int h : { 1, 2, 3, 5, 8, 13, 21, 34 })
                    {
                        const double f = h * sc.notes[ni].f0 * std::sqrt (1.0 + sc.config.inharmonicity * h * h);
                        if (f > 6000.0)
                            break;
                        const double q = std::max (8.0, 2.5 * h);
                        const auto er = analysis::envelopeDb (bandpass (ref, fs, f, q), fs, fr);
                        if (analysis::peakDb (er, fr, t0, t1) < -70.0)
                            continue;
                        const auto eo = analysis::envelopeDb (bandpass (out, fs, f, q), fs, fr);
                        const double rr = analysis::decayRateDbPerSec (er, fr, t0, t1, 20.0);
                        const double ro = analysis::decayRateDbPerSec (eo, fr, t0, t1, 20.0);
                        ++measured;
                        // Decays faster than 500 dB/s are over within ~40 ms, where the
                        // attack hold dominates the fit; allow more slack there.
                        const double lo = rr < -500.0 ? 0.5 : 0.65;
                        const double ratio = ro / rr;
                        CHECK (ratio > lo && ratio < 1.5,
                               "%s mute=%.2f damp=%.1f note %zu h%d (%.0f Hz): out %.0f dB/s, ref %.0f dB/s",
                               name, mute, damping, ni, h, f, ro, rr);
                    }
                }
            }
    std::printf ("  (%d harmonic decays measured)\n", measured);
}

// A sustained chord (beating strings) must keep decaying: no false re-triggers.
static void testSustainedChordStaysMuted()
{
    std::printf ("sustained chord: no false re-triggers\n");
    const double fs = 48000;
    const auto all = rbsynth::makeScenarios (fs);
    const auto& sc = scenario (all, "electric_chord_ring");
    rb::Params p;
    p.thump = 0.0f;
    const auto dry = rbsynth::render (sc.notes, sc.duration, sc.config, {});
    const auto ref = rbsynth::render (sc.notes, sc.duration, sc.config, targetFor (p));
    const auto out = processMono (dry, fs, p);
    const auto bo = analysis::splitBands (out, fs);
    const auto br = analysis::splitBands (ref, fs);
    for (int k = 0; k < analysis::kBands; ++k)
    {
        const auto eo = analysis::envelopeDb (bo[(size_t) k], fs);
        const auto er = analysis::envelopeDb (br[(size_t) k], fs);
        const double err = analysis::meanAbsEnvelopeErrorDb (eo, er);
        CHECK (err < 8.0, "band %d envelope error %.1f dB", k, err);
    }

    // Directly: after the strum has finished, no band gain may ever rise again.
    rb::RubberBridge dsp;
    dsp.prepare (fs, 1);
    dsp.setParams (p);
    dsp.reset();
    auto x = dry;
    std::array<float, rb::kNumBands> prev {};
    prev.fill (1000.0f);
    int reopenings = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        float* ptr = &x[i];
        dsp.process (&ptr, 1, 1);
        if ((double) i / fs < 0.3)
            continue;
        for (int k = 0; k < rb::kNumBands; ++k)
        {
            const float g = dsp.getBandGainDb (k);
            if (g > prev[(size_t) k] + 0.01f)
            {
                if (reopenings++ == 0)
                    std::printf ("  first re-opening: band %d at %.3fs (%.1f -> %.1f dB)\n", k, (double) i / fs, prev[(size_t) k], g);
            }
            prev[(size_t) k] = g;
        }
    }
    CHECK (reopenings == 0, "%d samples with a band gain re-opening during the sustained chord", reopenings);
}

// Plucks must keep their attack: the peak of the first 30 ms of every note must
// reach the ideal muted guitar's peak (comparing with dry would be wrong for
// re-picks: dry still contains the un-muted previous note).
static void testAttackPreserved()
{
    std::printf ("attack preserved (vs reference)\n");
    const double fs = 48000;
    const auto all = rbsynth::makeScenarios (fs);
    for (const auto& sc : all)
    {
        rb::Params p;
        p.thump = 0.0f;
        p.instrument = (rb::Instrument) sc.instrument;
        const auto dry = rbsynth::render (sc.notes, sc.duration, sc.config, {});
        const auto ref = rbsynth::render (sc.notes, sc.duration, sc.config, targetFor (p));
        const auto out = processMono (dry, fs, p);
        const double fr = 0.005;
        const auto er = analysis::envelopeDb (ref, fs, fr);
        const auto eo = analysis::envelopeDb (out, fs, fr);
        double prevT = -1.0;
        for (const auto& n : sc.notes)
        {
            if (n.start - prevT < 0.05)
                continue; // strum: only check the first string of each stroke
            prevT = n.start;
            const double pr = analysis::peakDb (er, fr, n.start, n.start + 0.03);
            const double po = analysis::peakDb (eo, fr, n.start, n.start + 0.03);
            // 3.5 dB: the zero-latency crossover delays the lows by a few ms, which
            // alone lowers a pluck's 5 ms peak by ~2-2.6 dB (same energy, see DESIGN.md).
            CHECK (po > pr - 3.5, "%s note @%.2fs: attack peak out %.1f dB vs ref %.1f dB", sc.name.c_str(), n.start, po, pr);
        }
    }
}

// Re-strums over ringing strings: every stroke must come through at the
// reference level (not attenuated by the previous stroke's muting).
static void testRestrum()
{
    std::printf ("re-strum levels vs reference\n");
    const double fs = 48000;
    const auto all = rbsynth::makeScenarios (fs);
    for (const char* name : { "acoustic_strum", "electric_riff" })
    {
        const auto& sc = scenario (all, name);
        rb::Params p;
        p.thump = 0.0f;
        p.instrument = (rb::Instrument) sc.instrument;
        const auto dry = rbsynth::render (sc.notes, sc.duration, sc.config, {});
        const auto ref = rbsynth::render (sc.notes, sc.duration, sc.config, targetFor (p));
        const auto out = processMono (dry, fs, p);
        const double fr = 0.005;
        const auto er = analysis::envelopeDb (ref, fs, fr);
        const auto eo = analysis::envelopeDb (out, fs, fr);
        double prevT = -1.0;
        for (const auto& n : sc.notes)
        {
            if (n.start - prevT < 0.05)
                continue;
            prevT = n.start;
            const double pr = analysis::peakDb (er, fr, n.start, n.start + 0.04);
            const double po = analysis::peakDb (eo, fr, n.start, n.start + 0.04);
            // Up to +4 dB allowed: strings that were not re-struck are re-opened
            // along with the new ones (Retrigger trade-off, see DESIGN.md).
            CHECK (po - pr > -3.5 && po - pr < 4.0, "%s stroke @%.2fs: out %.1f dB vs ref %.1f dB", name, n.start, po, pr);
        }
    }
}

// A melody played over a ringing bass note must not un-mute the bass.
static void testNoUnmuteOfRingingNotes()
{
    std::printf ("melody does not un-mute ringing bass (Retrigger 0)\n");
    const double fs = 48000;
    const auto all = rbsynth::makeScenarios (fs);
    const auto& sc = scenario (all, "melody_over_bass");
    rb::Params p;
    p.thump = 0.0f;
    p.retrigger = 0.0f;
    const auto dry = rbsynth::render (sc.notes, sc.duration, sc.config, {});
    const auto ref = rbsynth::render (sc.notes, sc.duration, sc.config, targetFor (p));
    const auto out = processMono (dry, fs, p);
    // Measure the bass note's own fundamental and 2nd harmonic (narrow band-pass,
    // so the melody notes don't leak into the measurement).
    for (int h : { 1, 2 })
    {
        const double f = h * sc.notes[0].f0;
        const auto bo = bandpass (out, fs, f, 10.0);
        const auto br = bandpass (ref, fs, f, 10.0);
        const auto bd = bandpass (dry, fs, f, 10.0);
        const size_t a = (size_t) (1.0 * fs), b = (size_t) (4.5 * fs);
        const double lo = rmsDb (bo, a, b), lr = rmsDb (br, a, b), ld = rmsDb (bd, a, b);
        CHECK (lo < lr + 10.0 && lo < ld - 15.0,
               "bass h%d 1-4.5 s: out %.1f dB, ref %.1f dB, dry %.1f dB (should stay muted)", h, lo, lr, ld);
    }
}

static void testOverallEnvelopeError()
{
    std::printf ("overall envelope error vs reference\n");
    const double fs = 48000;
    for (const auto& sc : rbsynth::makeScenarios (fs))
    {
        rb::Params p;
        p.thump = 0.0f;
        p.instrument = (rb::Instrument) sc.instrument;
        // Melody over a ringing bass is the fingerstyle case: use the
        // fingerstyle Retrigger setting the manual recommends.
        if (sc.name == "melody_over_bass")
            p.retrigger = 0.0f;
        const auto dry = rbsynth::render (sc.notes, sc.duration, sc.config, {});
        const auto ref = rbsynth::render (sc.notes, sc.duration, sc.config, targetFor (p));
        const auto out = processMono (dry, fs, p);
        const auto bo = analysis::splitBands (out, fs);
        const auto br = analysis::splitBands (ref, fs);
        const auto bd = analysis::splitBands (dry, fs);
        double eOut = 0, eDry = 0;
        for (int k = 0; k < analysis::kBands; ++k)
        {
            const auto er = analysis::envelopeDb (br[(size_t) k], fs);
            eOut += analysis::meanAbsEnvelopeErrorDb (analysis::envelopeDb (bo[(size_t) k], fs), er);
            eDry += analysis::meanAbsEnvelopeErrorDb (analysis::envelopeDb (bd[(size_t) k], fs), er);
        }
        eOut /= analysis::kBands;
        eDry /= analysis::kBands;
        CHECK (eOut < 0.35 * eDry && eOut < 7.0, "%s: mean envelope error out %.2f dB (dry %.2f dB)",
               sc.name.c_str(), eOut, eDry);
    }
}

// Automation torture: every parameter randomised every block (including the
// instrument switch). Output must stay finite and bounded, with no blow-ups.
static void testParameterAutomation()
{
    std::printf ("parameter automation stability\n");
    const double fs = 48000;
    const auto all = rbsynth::makeScenarios (fs);
    const auto& sc = scenario (all, "acoustic_strum");
    auto x = rbsynth::render (sc.notes, sc.duration, sc.config, {});
    std::vector<float> l = x, r = x;
    rb::RubberBridge dsp;
    dsp.prepare (fs, 2);
    dsp.reset();
    uint32_t seed = 7;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (float) (seed >> 8) / 16777216.0f; };
    double mx = 0.0;
    bool finite = true;
    for (size_t pos = 0; pos < l.size(); pos += 32)
    {
        rb::Params p;
        p.mute = rnd();
        p.damping = rnd();
        p.attack = rnd();
        p.retrigger = rnd();
        p.sensitivity = rnd();
        p.thump = rnd();
        p.mix = rnd();
        p.outputDb = -24.0f + 36.0f * rnd();
        p.instrument = (rb::Instrument) (int) (rnd() * 2.999f);
        dsp.setParams (p);
        float* ptrs[2] = { l.data() + pos, r.data() + pos };
        dsp.process (ptrs, 2, (int) std::min<size_t> (32, l.size() - pos));
    }
    for (size_t i = 0; i < l.size(); ++i)
    {
        finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]);
        mx = std::max ({ mx, (double) std::fabs (l[i]), (double) std::fabs (r[i]) });
    }
    double inMax = 0.0;
    for (float v : x)
        inMax = std::max (inMax, (double) std::fabs (v));
    CHECK (finite, "non-finite output under automation");
    // Max gain: +12 dB output, +6 dB thump.
    CHECK (mx < inMax * 4.0 * 2.0 * 1.5, "output peak %.3f vs input peak %.3f", mx, inMax);
}

static void testSampleRateConsistency()
{
    std::printf ("sample-rate consistency\n");
    double rates[3];
    int idx = 0;
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const auto all = rbsynth::makeScenarios (fs);
        const auto& sc = scenario (all, "electric_open_strings");
        rb::Params p;
        p.thump = 0.0f;
        const auto dry = rbsynth::render (sc.notes, sc.duration, sc.config, {});
        const auto out = processMono (dry, fs, p);
        const auto b = analysis::splitBands (out, fs);
        const auto e = analysis::envelopeDb (b[2], fs, 0.005);
        rates[idx++] = analysis::decayRateDbPerSec (e, 0.005, sc.notes[2].start, sc.notes[3].start, 25.0);
    }
    CHECK (std::fabs (rates[0] / rates[1] - 1.0) < 0.1 && std::fabs (rates[2] / rates[1] - 1.0) < 0.1,
           "decay rates differ across sample rates: %.1f %.1f %.1f dB/s", rates[0], rates[1], rates[2]);
}

int main()
{
    testTransparency();
    testSilenceAndStability();
    testBlockSizeIndependence();
    testStereoLinked();
    testDecayMatchesPhysics();
    testSustainedChordStaysMuted();
    testAttackPreserved();
    testRestrum();
    testNoUnmuteOfRingingNotes();
    testOverallEnvelopeError();
    testSampleRateConsistency();
    testParameterAutomation();

    std::printf ("\n%d/%d checks passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
