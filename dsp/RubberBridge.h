#pragma once
// Rubber Bridge: a zero-latency "soft bridge termination" emulation.
//
// Physical idea
// -------------
// A rubber (or foam) bridge terminates the string on a lossy, compliant
// support. That adds damping on top of the string's natural losses, and the
// added damping grows with frequency: high partials die almost instantly, the
// fundamental survives a little longer as a short "thunk".
//
// For an exponentially decaying partial, decay *rates* add:
//     rate_total(f) = rate_natural(f) + rate_bridge(f),   rate_bridge(f) = R * (f / 1 kHz)^alpha
// so the bridge can be applied as an extra per-frequency exponential decay
// that restarts on every pluck. That is what this processor does:
//
//   1. Split the signal into 8 bands (Linkwitz-Riley 4th order, all-pass
//      compensated, so the bands sum flat when untouched).
//   2. Each band has a gain g_k that decays at rate_bridge(f_k) while the
//      note rings. The output therefore keeps the instrument's own decay
//      character, multiplied by the bridge's extra loss.
//   3. A pluck detector re-opens the gains. It compares each band's peak
//      follower with the band's own peaks over the previous ~2.4 periods of
//      the lowest note (a ringing note never exceeds its recent peaks; a new
//      pluck does) and sums the rises over the upper bands. How far the low
//      bands re-open is set by "Retrigger", because a melody note over a
//      ringing bass and a re-picked bass look identical in the low bands.
//
// See docs/DESIGN.md for the measurements behind the constants.
//
// Everything is IIR and causal, so the plugin reports zero latency.

#include "Filters.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace rb
{

enum class Instrument : int
{
    Electric = 0,
    Acoustic = 1,
    Baritone = 2
};

struct Params
{
    float mute = 0.55f;      // 0..1   amount of extra bridge damping
    float damping = 0.5f;    // 0..1   how much faster highs die than lows (tilt)
    float attack = 0.5f;     // 0..1   pick snap: 0.5 = physical, lower = softer/darker, higher = longer hold
    float retrigger = 0.8f;  // 0..1   how fully a new pluck re-opens the low bands
    float sensitivity = 0.5f; // 0..1  pluck detection sensitivity
    float thump = 0.3f;      // 0..1   low-mid body knock on each pluck
    float mix = 1.0f;        // 0..1   dry/wet (phase coherent)
    float outputDb = 0.0f;   // dB
    Instrument instrument = Instrument::Electric;
};

constexpr int kNumBands = 8;

struct InstrumentConfig
{
    std::array<double, kNumBands - 1> crossovers; // Hz
    double lowestNoteHz;                          // sets detector windows (envelope periodicity)
};

inline InstrumentConfig getInstrumentConfig (Instrument i)
{
    switch (i)
    {
        case Instrument::Acoustic: return { { 135.0, 240.0, 430.0, 780.0, 1400.0, 2500.0, 4500.0 }, 70.0 };
        case Instrument::Baritone: return { { 90.0, 160.0, 290.0, 520.0, 950.0, 1700.0, 3100.0 }, 44.0 };
        case Instrument::Electric:
        default:                   return { { 125.0, 220.0, 400.0, 720.0, 1300.0, 2300.0, 4200.0 }, 70.0 };
    }
}

class RubberBridge
{
public:
    static constexpr int kMaxChannels = 2;

    // Mapping constants (documented in docs/DESIGN.md).
    static constexpr double kMaxRateAt1k = 100.0; // nepers/s at Mute=100% (T60 ~ 69 ms at 1 kHz)
    static constexpr double kGainFloor = 1.0e-5;  // -100 dB

    RubberBridge() { prepare (48000.0, 2); } // usable (and reports band centres) before prepare()

    static double bridgeRateAt1k (double mute) { return kMaxRateAt1k * std::pow (std::clamp (mute, 0.0, 1.0), 1.5); }
    static double tiltExponent (double damping) { return 0.2 + 1.0 * std::clamp (damping, 0.0, 1.0); }

    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        configure();
        reset();
    }

    void reset()
    {
        for (auto& ch : chState)
        {
            for (auto& s : ch.splits) s.reset();
            for (auto& a : ch.allpass) a.reset();
        }
        for (auto& b : bands)
        {
            b.p1 = b.p2 = b.env = 0.0;
            b.g = 1.0;
            b.thumpEnv = 0.0;
            b.fast = 0.0;
            b.segMax.fill (0.0);
            b.curMax = 0.0;
            b.push = 0.0;
            b.lastGain.store (1.0f);
        }
        segPos = 0;
        holdCounter = 0;
        mixSmoothed = targetMix;
        outGainSmoothed = targetOutGain;
    }

    void setParams (const Params& p)
    {
        const bool instrumentChanged = p.instrument != params.instrument;
        params = p;
        if (instrumentChanged)
            configure();
        else
            updateParamDerived();
    }

    const Params& getParams() const { return params; }

    // Current gain of a band in dB (for UI meters, safe to read from any thread).
    float getBandGainDb (int band) const
    {
        return (float) gainToDb (bands[(size_t) band].lastGain.load (std::memory_order_relaxed));
    }

    double getBandCentreHz (int band) const { return bands[(size_t) band].centreHz; }

    // Diagnostics (last processed sample).
    double getLastFluxDb() const { return lastFlux; }
    double getBandEnvDb (int band) const { return gainToDb (bands[(size_t) band].env); }
    double getLastConfidence() const { return lastConf; }
    const std::array<double, kNumBands>& getLastRiseDb() const { return lastRise; }

    // In-place processing. channelData[ch][i], numChannels <= 2.
    void process (float* const* channelData, int numChannels, int numSamples)
    {
        const int nch = std::min (numChannels, channels);
        const double invCh = 1.0 / (double) nch;

        const double mixStep = (targetMix - mixSmoothed) / std::max (1, numSamples);
        const double outStep = (targetOutGain - outGainSmoothed) / std::max (1, numSamples);

        std::array<std::array<double, kNumBands>, kMaxChannels> x {};

        for (int i = 0; i < numSamples; ++i)
        {
            mixSmoothed += mixStep;
            outGainSmoothed += outStep;

            // ---- Band split (per channel) ------------------------------------
            for (int ch = 0; ch < nch; ++ch)
                splitBands (chState[(size_t) ch], channelData[ch][i], x[(size_t) ch]);

            // ---- Per-band envelopes and pluck detection -----------------------
            std::array<double, kNumBands> riseDb {};
            double flux = 0.0;
            for (int k = 0; k < kNumBands; ++k)
            {
                auto& b = bands[(size_t) k];
                double p = 0.0, peak = 0.0;
                for (int ch = 0; ch < nch; ++ch)
                {
                    const double v = x[(size_t) ch][(size_t) k];
                    p += v * v;
                    peak = std::max (peak, std::fabs (v));
                }
                p *= invCh;

                // Smooth amplitude envelope (for the gain computer).
                b.p1 += envCoeff * (p - b.p1);
                b.p2 += envCoeff * (b.p1 - b.p2);
                b.env = std::sqrt (std::max (b.p2, 0.0));

                // Peak follower vs. held peak of the previous ~2.4 periods of
                // the lowest note. A ringing note never exceeds its own recent
                // peaks; a new pluck does.
                b.fast = peak > b.fast ? peak : b.fast * b.fastRelease;
                b.curMax = std::max (b.curMax, b.fast);
                const double held = *std::max_element (b.segMax.begin(), b.segMax.end());
                if (b.fast > kEnvFloor)
                {
                    const double r = gainToDb (b.fast / std::max (held, kEnvFloor * 0.1));
                    riseDb[(size_t) k] = std::max (0.0, r);
                    if (k >= firstFluxBand)
                        flux += std::min (riseDb[(size_t) k], kMaxRisePerBandDb);
                }
            }
            if (++segPos >= segLen)
            {
                segPos = 0;
                for (auto& b : bands)
                {
                    for (size_t j = kNumSegments - 1; j > 0; --j)
                        b.segMax[j] = b.segMax[j - 1];
                    b.segMax[0] = b.curMax;
                    b.curMax = 0.0;
                }
            }

            const double conf = std::clamp ((flux - fluxThresholdDb) / kFluxRangeDb, 0.0, 1.0);
            lastFlux = flux;
            lastConf = conf;
            lastRise = riseDb;

            if (conf > 0.0)
            {
                if (holdCounter == 0)
                    for (auto& b : bands)
                        b.push = 0.0;
                holdCounter = holdSamples;
                for (int k = 0; k < kNumBands; ++k)
                {
                    auto& b = bands[(size_t) k];
                    // Bands that rose themselves always re-open; the rest re-open
                    // according to Retrigger (always fully in the upper bands).
                    const double own = std::clamp ((riseDb[(size_t) k] - kOwnRiseDeadZoneDb)
                                                       / (kOwnRiseFullDb - kOwnRiseDeadZoneDb), 0.0, 1.0);
                    const double weight = std::max (own, b.retriggerWeight);
                    b.push = std::max (b.push, conf * weight * b.attackWeight);
                }
            }
            else if (holdCounter > 0 && --holdCounter == 0)
            {
                for (auto& b : bands)
                    b.push = 0.0;
            }

            // ---- Per-band gain computers (stereo linked) ----------------------
            // A real gap between notes (every band silent): nothing to mute, so
            // drift back open, ready for the next note.
            bool silent = true;
            for (const auto& b : bands)
                silent = silent && b.env < kEnvFloor;

            std::array<double, kNumBands> G {};
            for (int k = 0; k < kNumBands; ++k)
            {
                auto& b = bands[(size_t) k];
                if (silent)
                    b.g += (1.0 - b.g) * silenceRelax;
                else
                    b.g *= b.decayCoeff;

                if (b.g < b.push)
                    b.g += (b.push - b.g) * resetCoeff;

                b.g = std::clamp (b.g, kGainFloor, 1.0);

                // Thump: short low-mid boost following each pluck.
                b.thumpEnv = std::max (b.thumpEnv * thumpDecay, b.push);
                G[(size_t) k] = b.g * (1.0 + thumpAmount * b.thumpShape * b.thumpEnv);
            }

            // ---- Recombine ----------------------------------------------------
            // The LR4 tree sums to an all-pass when all gains are equal, so Mix
            // is phase coherent (dry is taken from the same bands).
            const double wet = mixSmoothed;
            const double dry = 1.0 - wet;
            for (int ch = 0; ch < nch; ++ch)
            {
                double y = 0.0;
                for (int k = 0; k < kNumBands; ++k)
                    y += x[(size_t) ch][(size_t) k] * (dry + wet * G[(size_t) k]);
                channelData[ch][i] = (float) (y * outGainSmoothed);
            }
        }

        for (auto& b : bands)
        {
            b.lastGain.store ((float) b.g, std::memory_order_relaxed);
            // Denormal safety on any platform.
            if (b.p1 < 1.0e-30) b.p1 = 0.0;
            if (b.p2 < 1.0e-30) b.p2 = 0.0;
            if (b.fast < 1.0e-30) b.fast = 0.0;
        }
    }

private:
    static constexpr double kEnvFloor = 3.0e-5;          // ~ -90 dBFS
    static constexpr double kFluxRangeDb = 1.0;          // confidence ramp above the threshold
    static constexpr double kOwnRiseDeadZoneDb = 1.5;    // a band's own rise below this doesn't count
    static constexpr double kOwnRiseFullDb = 6.0;
    static constexpr double kMaxRisePerBandDb = 12.0;
    static constexpr double kFluxMinBandHz = 250.0;

    static constexpr int kNumAllpasses = (kNumBands - 2) * (kNumBands - 1) / 2;
    static constexpr size_t kNumSegments = 4; // held-peak window = 4 x 0.6 periods of the lowest note

    struct ChannelState
    {
        std::array<LR4Split, kNumBands - 1> splits;
        std::array<SvfState, kNumAllpasses> allpass;
    };

    struct BandState
    {
        // gain computer
        double p1 = 0.0, p2 = 0.0, env = 0.0;
        double g = 1.0;
        double thumpEnv = 0.0;

        // pluck detector
        double fast = 0.0, fastRelease = 0.99;
        std::array<double, kNumSegments> segMax {};
        double curMax = 0.0;
        double push = 0.0;

        // derived parameters
        double lowEdgeHz = 70.0, centreHz = 1000.0;
        double decayCoeff = 1.0;
        double attackWeight = 1.0;
        double retriggerWeight = 1.0;
        double thumpShape = 0.0;
        std::atomic<float> lastGain { 1.0f };
    };

    void splitBands (ChannelState& cs, double in, std::array<double, kNumBands>& out)
    {
        double rest = in;
        for (int s = 0; s < kNumBands - 1; ++s)
        {
            double low, high;
            cs.splits[(size_t) s].process (xover[(size_t) s], rest, low, high);
            out[(size_t) s] = low;
            rest = high;
        }
        out[kNumBands - 1] = rest;

        // Phase-align each lower band with the splits that came after it.
        size_t a = 0;
        for (int k = 0; k < kNumBands - 2; ++k)
            for (int s = k + 1; s < kNumBands - 1; ++s)
                out[(size_t) k] = cs.allpass[a++].allpass (xover[(size_t) s], out[(size_t) k]);
    }

    void configure()
    {
        const auto cfg = getInstrumentConfig (params.instrument);
        constexpr double butterworthQ = 0.7071067811865476;
        for (int s = 0; s < kNumBands - 1; ++s)
            xover[(size_t) s].set (cfg.crossovers[(size_t) s], butterworthQ, fs);

        const double topEdge = std::min (10000.0, 0.45 * fs);
        firstFluxBand = kNumBands - 1;
        for (int k = 0; k < kNumBands; ++k)
        {
            auto& b = bands[(size_t) k];
            b.lowEdgeHz = k == 0 ? cfg.lowestNoteHz : cfg.crossovers[(size_t) k - 1];
            const double hi = k == kNumBands - 1 ? topEdge : cfg.crossovers[(size_t) k];
            b.centreHz = std::sqrt (b.lowEdgeHz * hi);
            // Peak follower release ~ one carrier period at the band's low edge.
            b.fastRelease = std::exp (-b.lowEdgeHz / fs);
            if (b.lowEdgeHz >= kFluxMinBandHz)
                firstFluxBand = std::min (firstFluxBand, k);
        }

        // Harmonics of the lowest note beat at f0 in every band, so smooth well
        // below it. Two poles at f0/4 -> ~24 dB ripple rejection.
        envCoeff = onePoleCoeffHz (cfg.lowestNoteHz / 4.0, fs);

        // Held-peak window: 2 segments of 0.6 periods of the lowest note.
        segLen = std::max (1, (int) std::ceil (0.6 * fs / cfg.lowestNoteHz));

        resetCoeff = onePoleCoeff (0.00004, fs);   // ~2 samples: open before the pick click is over
        silenceRelax = onePoleCoeff (0.050, fs);
        thumpDecay = std::exp (-1.0 / (0.040 * fs));

        updateParamDerived();
    }

    void updateParamDerived()
    {
        const double mute = std::clamp ((double) params.mute, 0.0, 1.0);
        const double attack = std::clamp ((double) params.attack, 0.0, 1.0);
        const double retrig = std::clamp ((double) params.retrigger, 0.0, 1.0);

        const double R = bridgeRateAt1k (mute);
        const double alpha = tiltExponent (params.damping);

        // Attack: how long a pluck holds the bands open, and how much of the
        // top end is admitted.
        // 50% = physical (no extra HF loss, ~2 ms hold). Below: softer, darker
        // plucks. Above: the pluck is held open longer for more pick snap.
        const double snap = std::max (0.0, 2.0 * attack - 1.0);
        holdSamples = std::max (1, (int) ((0.002 + 0.028 * snap * snap) * fs));
        const double topWeight = std::min (1.0, 0.4 + 1.2 * attack);
        const double muteBlend = std::min (1.0, 2.0 * mute); // no tone change at Mute=0

        for (int k = 0; k < kNumBands; ++k)
        {
            auto& b = bands[(size_t) k];
            const double rate = R * std::pow (b.centreHz / 1000.0, alpha);
            b.decayCoeff = std::exp (-rate / fs);

            // HF admit: bands above ~1 kHz lose up to (1 - topWeight).
            const double pos = std::clamp (std::log2 (b.centreHz / 1000.0) / 3.0, 0.0, 1.0);
            const double w = 1.0 - (1.0 - topWeight) * pos;
            b.attackWeight = 1.0 - (1.0 - w) * muteBlend;

            // Retrigger applies below ~400 Hz; above, every pluck re-opens fully.
            const double low = std::clamp (std::log2 (400.0 / b.centreHz) / 1.5, 0.0, 1.0);
            b.retriggerWeight = 1.0 - (1.0 - retrig) * low;

            const double oct = std::log2 (b.centreHz / 250.0);
            b.thumpShape = std::exp (-0.5 * oct * oct / 0.8);
        }

        // Sensitivity: summed rise (dB over the upper bands) needed to count as a pluck.
        // 0.5 -> 4.75 dB, which separates soft re-picks from chord beating on the
        // synthetic tests (see docs/DESIGN.md).
        fluxThresholdDb = 7.25 - 5.0 * std::clamp ((double) params.sensitivity, 0.0, 1.0);

        thumpAmount = std::clamp ((double) params.thump, 0.0, 1.0); // up to +6 dB
        targetMix = std::clamp ((double) params.mix, 0.0, 1.0);
        targetOutGain = dbToGain (params.outputDb);
    }

    double fs = 48000.0;
    int channels = 2;
    Params params;

    std::array<SvfCoeffs, kNumBands - 1> xover;
    std::array<ChannelState, kMaxChannels> chState;
    std::array<BandState, kNumBands> bands;

    double fluxThresholdDb = 4.5;
    double lastFlux = 0.0, lastConf = 0.0;
    std::array<double, kNumBands> lastRise {};
    int firstFluxBand = 3;
    int segLen = 400, segPos = 0;
    int holdSamples = 480, holdCounter = 0;
    double envCoeff = 0.01;
    double silenceRelax = 0.001;
    double resetCoeff = 0.1, thumpDecay = 0.999, thumpAmount = 0.0;
    double targetMix = 1.0, mixSmoothed = 1.0;
    double targetOutGain = 1.0, outGainSmoothed = 1.0;
};

} // namespace rb
