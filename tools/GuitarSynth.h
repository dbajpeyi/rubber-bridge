#pragma once
// Modal plucked-string synthesiser used to test the plugin without recordings.
//
// It renders the same performance twice:
//   * "dry":       an ordinary bridge (natural string losses only)
//   * "reference": the same strings with extra rubber-bridge damping applied
//                  per partial using the physical law rate_total = rate_nat + rate_bridge(f)
//
// The reference is the ideal output of a perfect rubber-bridge processor, so
// the plugin's output can be compared against it objectively.

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace rbsynth
{

constexpr double kPi = 3.14159265358979323846;

enum class Body { Electric, Acoustic };

struct Note
{
    double start;     // seconds
    double f0;        // Hz
    double velocity;  // 0..1
    int string;       // a new note on the same string stops the previous one
    double pluckPos = 0.18; // relative distance from the bridge
};

struct Damping
{
    double ratePer1k = 0.0; // extra decay rate at 1 kHz (nepers/s); 0 = ordinary bridge
    double alpha = 0.7;     // tilt exponent
};

struct SynthConfig
{
    double sampleRate = 48000.0;
    Body body = Body::Electric;
    double inharmonicity = 1.0e-4;
    double pickupPos = 0.09; // electric pickup position (relative), comb filtering
    double pickClick = 0.35; // plectrum transient level (fingers: ~0.1)
    uint32_t seed = 1234;
};

// Natural (ordinary bridge) decay rate of a partial, nepers/s.
// T60 ~ 6 s at 82 Hz, ~2.6 s at 1 kHz, ~0.9 s at 5 kHz.
inline double naturalRate (double f)
{
    const double t60 = 7.0 / std::pow (1.0 + f / 400.0, 0.8);
    return 6.907755 / t60;
}

inline double bridgeRate (const Damping& d, double f)
{
    return d.ratePer1k <= 0.0 ? 0.0 : d.ratePer1k * std::pow (f / 1000.0, d.alpha);
}

inline std::vector<float> render (const std::vector<Note>& notes, double duration,
                                  const SynthConfig& cfg, const Damping& damping)
{
    const double fs = cfg.sampleRate;
    const size_t n = (size_t) std::ceil (duration * fs);
    std::vector<double> out (n, 0.0);
    std::mt19937 rng (cfg.seed);
    std::uniform_real_distribution<double> uni (0.0, 1.0);
    std::normal_distribution<double> gauss (0.0, 1.0);

    for (size_t ni = 0; ni < notes.size(); ++ni)
    {
        const Note& note = notes[ni];

        // End time: next note on the same string (string re-plucked / fretted).
        double end = duration;
        for (size_t nj = 0; nj < notes.size(); ++nj)
            if (nj != ni && notes[nj].string == note.string && notes[nj].start > note.start)
                end = std::min (end, notes[nj].start);

        const size_t s0 = (size_t) std::floor (note.start * fs);
        const size_t s1 = std::min (n, (size_t) std::ceil (end * fs));
        // A re-pick first stops the string: the pick contacts it for a few ms.
        const size_t fadeLen = (size_t) (0.008 * fs);
        const double amp = 0.25 * note.velocity;
        // Harder plucks are brighter.
        const double brightness = 0.6 + 0.8 * note.velocity;

        for (int h = 1; h < 200; ++h)
        {
            const double fh = h * note.f0 * std::sqrt (1.0 + cfg.inharmonicity * h * h);
            if (fh > std::min (9000.0, 0.45 * fs))
                break;

            double a = std::sin (h * kPi * note.pluckPos) / std::pow ((double) h, 1.25 - 0.35 * brightness);
            if (cfg.body == Body::Electric)
                a *= std::fabs (std::sin (h * kPi * cfg.pickupPos)) * 3.0;
            a *= amp;
            if (std::fabs (a) < 1.0e-5)
                continue;

            // Two polarisations: slightly detuned, slightly different losses.
            for (int pol = 0; pol < 2; ++pol)
            {
                const double f = fh * (pol == 0 ? 1.0 : 1.0007);
                const double ap = pol == 0 ? a : 0.35 * a;
                const double rate = naturalRate (f) * (pol == 0 ? 1.0 : 1.4) + bridgeRate (damping, f);
                const double decay = std::exp (-rate / fs);
                const double w = 2.0 * kPi * f / fs;
                const double cw = std::cos (w), sw = std::sin (w);
                // A plucked string starts from rest: all modes start in phase (with a
                // little jitter), which makes the waveform's top end spiky.
                const double phase = 0.15 * (uni (rng) - 0.5);
                double re = std::cos (phase) * ap, im = std::sin (phase) * ap;

                for (size_t s = s0; s < s1; ++s)
                {
                    double env = 1.0;
                    if (s1 - s < fadeLen)
                        env = (double) (s1 - s) / (double) fadeLen; // damped by next pluck
                    out[s] += im * env;
                    const double nre = (re * cw - im * sw) * decay;
                    const double nim = (re * sw + im * cw) * decay;
                    re = nre;
                    im = nim;
                    if ((s & 255) == 0 && re * re + im * im < 1.0e-14)
                        break;
                }
            }
        }

        // Pick / finger transient: short broadband click (bridge independent).
        // Plectrum attacks are mostly above the string's own spectrum; soft
        // finger plucks (low pluckPos-independent velocity) are much weaker.
        {
            const size_t len = (size_t) (0.004 * fs);
            double hp1 = 0.0, prev1 = 0.0, hp2 = 0.0, prev2 = 0.0;
            const double hpA = std::exp (-2.0 * kPi * 1500.0 / fs);
            for (size_t s = 0; s < len && s0 + s < n; ++s)
            {
                const double env = std::exp (-(double) s / (0.0007 * fs));
                const double v = gauss (rng);
                hp1 = hpA * (hp1 + v - prev1);
                prev1 = v;
                hp2 = hpA * (hp2 + hp1 - prev2);
                prev2 = hp1;
                out[s0 + s] += cfg.pickClick * amp * note.velocity * env * hp2;
            }
        }

        // Acoustic body: low modes excited by every pluck (short, bridge independent).
        if (cfg.body == Body::Acoustic)
        {
            const double modes[3][3] = { { 105.0, 0.18, 0.08 }, { 210.0, 0.12, 0.05 }, { 390.0, 0.07, 0.03 } };
            for (const auto& m : modes)
            {
                const double decay = std::exp (-1.0 / (m[2] * fs));
                const double w = 2.0 * kPi * m[0] / fs;
                const double cw = std::cos (w), sw = std::sin (w);
                double re = 0.0, im = m[1] * amp;
                for (size_t s = s0; s < n; ++s)
                {
                    out[s] += im;
                    const double nre = (re * cw - im * sw) * decay;
                    const double nim = (re * sw + im * cw) * decay;
                    re = nre;
                    im = nim;
                    if ((s & 255) == 0 && re * re + im * im < 1.0e-14)
                        break;
                }
            }
        }
    }

    std::vector<float> f (n);
    for (size_t i = 0; i < n; ++i)
        f[i] = (float) out[i];
    return f;
}

inline double midiToHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

struct Scenario
{
    std::string name;
    SynthConfig config;
    std::vector<Note> notes;
    double duration;
    int instrument; // matches rb::Instrument
};

// Strings (low -> high) as MIDI notes.
inline const std::vector<int>& standardTuning() { static std::vector<int> t { 40, 45, 50, 55, 59, 64 }; return t; }
inline const std::vector<int>& baritoneTuning() { static std::vector<int> t { 35, 40, 45, 50, 54, 59 }; return t; }

inline std::vector<Scenario> makeScenarios (double fs)
{
    std::vector<Scenario> sc;

    auto cfgFor = [fs] (Body body, double inharm)
    {
        SynthConfig c;
        c.sampleRate = fs;
        c.body = body;
        c.inharmonicity = inharm;
        return c;
    };

    // 1. Electric: each open string, let it ring.
    {
        Scenario s { "electric_open_strings", cfgFor (Body::Electric, 1.0e-4), {}, 9.5, 0 };
        for (int i = 0; i < 6; ++i)
            s.notes.push_back ({ 0.2 + 1.5 * i, midiToHz (standardTuning()[(size_t) i]), 0.8, i });
        sc.push_back (s);
    }
    // 2. Baritone (B standard): each open string.
    {
        Scenario s { "baritone_open_strings", cfgFor (Body::Electric, 0.6e-4), {}, 9.5, 2 };
        for (int i = 0; i < 6; ++i)
            s.notes.push_back ({ 0.2 + 1.5 * i, midiToHz (baritoneTuning()[(size_t) i]), 0.8, i });
        sc.push_back (s);
    }
    // 3. Acoustic: G major strummed, down/up eighths at 120 bpm (re-strum over ringing strings).
    {
        Scenario s { "acoustic_strum", cfgFor (Body::Acoustic, 0.8e-4), {}, 5.0, 1 };
        const int chord[6] = { 43, 47, 50, 55, 59, 67 }; // G2 B2 D3 G3 B3 G4
        for (int k = 0; k < 16; ++k)
        {
            const double t = 0.2 + 0.25 * k;
            const bool down = (k % 2) == 0;
            const double vel = down ? 0.85 : 0.6;
            for (int st = 0; st < 6; ++st)
            {
                const int idx = down ? st : 5 - st;
                if (! down && idx < 2)
                    continue; // upstrokes catch the top strings
                s.notes.push_back ({ t + 0.007 * (down ? st : (5 - idx)), midiToHz (chord[idx]), vel, idx });
            }
        }
        sc.push_back (s);
    }
    // 4. Electric riff: 16ths on the low E with accents (same string re-picked).
    {
        Scenario s { "electric_riff", cfgFor (Body::Electric, 1.0e-4), {}, 4.5, 0 };
        const int pattern[8] = { 40, 40, 43, 40, 45, 40, 47, 45 };
        for (int k = 0; k < 32; ++k)
            s.notes.push_back ({ 0.2 + 0.125 * k, midiToHz (pattern[k % 8]), (k % 4 == 0) ? 0.9 : 0.6, 0 });
        sc.push_back (s);
    }
    // 5. Melody over a ringing low string (new notes must not un-mute the old one).
    {
        Scenario s { "melody_over_bass", cfgFor (Body::Electric, 1.0e-4), {}, 5.0, 0 };
        s.notes.push_back ({ 0.2, midiToHz (40), 0.9, 0 });
        const int mel[8] = { 64, 67, 69, 71, 72, 71, 69, 67 };
        for (int k = 0; k < 8; ++k)
            s.notes.push_back ({ 0.8 + 0.3 * k, midiToHz (mel[k]), 0.7, 5 });
        sc.push_back (s);
    }
    // 6. One strummed E major chord left to ring (sustained beating must not re-open the mute).
    {
        Scenario s { "electric_chord_ring", cfgFor (Body::Electric, 1.0e-4), {}, 4.0, 0 };
        const int chord[6] = { 40, 47, 52, 56, 59, 64 };
        for (int st = 0; st < 6; ++st)
            s.notes.push_back ({ 0.2 + 0.008 * st, midiToHz (chord[st]), 0.8, st });
        sc.push_back (s);
    }
    // 7. Soft fingerpicked acoustic arpeggio (weak attacks).
    {
        Scenario s { "acoustic_fingerpick", cfgFor (Body::Acoustic, 0.8e-4), {}, 5.0, 1 };
        s.config.pickClick = 0.1;
        const int chord[6] = { 40, 47, 52, 55, 59, 64 }; // Em
        const int order[8] = { 0, 3, 4, 5, 2, 4, 3, 5 };
        for (int k = 0; k < 16; ++k)
        {
            const int idx = order[k % 8];
            s.notes.push_back ({ 0.2 + 0.28 * k, midiToHz (chord[idx]), 0.35, idx, 0.25 });
        }
        sc.push_back (s);
    }
    return sc;
}

} // namespace rbsynth
