#pragma once
// Small, allocation-free filter building blocks used by the Rubber Bridge DSP.
// Everything here is IIR and causal: zero latency.

#include <cmath>

namespace rb
{

constexpr double kPi = 3.14159265358979323846;

// Topology-preserving-transform state variable filter (Zavalishin / Simper).
// One tick yields low-pass, band-pass and high-pass outputs simultaneously.
struct SvfCoeffs
{
    double k = 1.4142135623730951; // 1/Q
    double a1 = 0.0, a2 = 0.0, a3 = 0.0;

    void set (double cutoffHz, double q, double sampleRate)
    {
        const double fc = std::fmin (cutoffHz, 0.49 * sampleRate);
        const double g = std::tan (kPi * fc / sampleRate);
        k = 1.0 / q;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
};

struct SvfState
{
    double ic1 = 0.0, ic2 = 0.0;

    void reset() { ic1 = ic2 = 0.0; }

    inline void tick (const SvfCoeffs& c, double x, double& lp, double& bp, double& hp)
    {
        const double v3 = x - ic2;
        const double v1 = c.a1 * ic1 + c.a2 * v3;
        const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = x - c.k * v1 - v2;
    }

    // 2nd-order all-pass: lp - k*bp + hp == x - 2k*bp
    inline double allpass (const SvfCoeffs& c, double x)
    {
        double lp, bp, hp;
        tick (c, x, lp, bp, hp);
        return x - 2.0 * c.k * bp;
    }

    inline double highpass (const SvfCoeffs& c, double x)
    {
        double lp, bp, hp;
        tick (c, x, lp, bp, hp);
        return hp;
    }
};

// 4th-order Linkwitz-Riley split. LP4 + HP4 sums to a 2nd-order Butterworth
// all-pass, so a crossover tree with matching all-pass compensation is
// magnitude-flat when all band gains are 1.
struct LR4Split
{
    SvfState s1, sLp, sHp;

    void reset() { s1.reset(); sLp.reset(); sHp.reset(); }

    inline void process (const SvfCoeffs& c, double x, double& low, double& high)
    {
        double lp1, bp1, hp1, lp2, bp2, hp2;
        s1.tick (c, x, lp1, bp1, hp1);
        sLp.tick (c, lp1, lp2, bp2, hp2);
        low = lp2;
        sHp.tick (c, hp1, lp2, bp2, hp2);
        high = hp2;
    }
};

// One-pole smoothing coefficient for a given time constant in seconds.
inline double onePoleCoeff (double timeConstantSec, double sampleRate)
{
    if (timeConstantSec <= 0.0)
        return 1.0;
    return 1.0 - std::exp (-1.0 / (timeConstantSec * sampleRate));
}

// One-pole smoothing coefficient for a given cutoff frequency.
inline double onePoleCoeffHz (double cutoffHz, double sampleRate)
{
    return 1.0 - std::exp (-2.0 * kPi * cutoffHz / sampleRate);
}

inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }
inline double gainToDb (double g) { return 20.0 * std::log10 (std::fmax (g, 1.0e-12)); }

} // namespace rb
