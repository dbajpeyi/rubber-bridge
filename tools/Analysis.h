#pragma once
// Objective measurements used by the tests and rb_analyze.

#include "../dsp/Filters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace analysis
{

constexpr int kBands = 5;

// Fixed analysis bands (independent of the plugin's instrument presets).
inline const std::array<double, kBands - 1>& analysisCrossovers()
{
    static std::array<double, kBands - 1> c { 160.0, 450.0, 1300.0, 3600.0 };
    return c;
}

inline std::array<std::vector<float>, kBands> splitBands (const std::vector<float>& x, double fs)
{
    std::array<rb::SvfCoeffs, kBands - 1> c;
    for (int i = 0; i < kBands - 1; ++i)
        c[(size_t) i].set (analysisCrossovers()[(size_t) i], 0.7071067811865476, fs);
    std::array<rb::LR4Split, kBands - 1> s;
    std::array<std::vector<float>, kBands> out;
    for (auto& b : out)
        b.resize (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        double v = x[i], lo, hi;
        for (int k = 0; k < kBands - 1; ++k)
        {
            s[(size_t) k].process (c[(size_t) k], v, lo, hi);
            out[(size_t) k][i] = (float) lo;
            v = hi;
        }
        out[kBands - 1][i] = (float) v;
    }
    return out;
}

// RMS envelope in dB with a given frame length (seconds).
inline std::vector<double> envelopeDb (const std::vector<float>& x, double fs, double frameSec = 0.01)
{
    const size_t hop = std::max<size_t> (1, (size_t) (frameSec * fs));
    std::vector<double> env;
    for (size_t i = 0; i + hop <= x.size(); i += hop)
    {
        double e = 0.0;
        for (size_t j = 0; j < hop; ++j)
            e += (double) x[i + j] * x[i + j];
        env.push_back (10.0 * std::log10 (e / (double) hop + 1.0e-20));
    }
    return env;
}

// Decay rate (dB/s) of a single note in [t0, t1): least-squares fit from
// the first frame 3 dB below the peak down to `spanDb` below the peak.
inline double decayRateDbPerSec (const std::vector<double>& envDb, double frameSec, double t0, double t1,
                                 double spanDb = 30.0)
{
    const size_t a = (size_t) (t0 / frameSec);
    const size_t b = std::min (envDb.size(), (size_t) (t1 / frameSec));
    if (b <= a + 3)
        return 0.0;
    size_t pk = a;
    for (size_t i = a; i < b; ++i)
        if (envDb[i] > envDb[pk])
            pk = i;
    const double peak = envDb[pk];
    std::vector<double> xs, ys;
    bool started = false;
    for (size_t i = pk; i < b; ++i)
    {
        if (! started && envDb[i] < peak - 3.0)
            started = true;
        if (! started)
            continue;
        if (envDb[i] < peak - spanDb || envDb[i] < -110.0)
            break;
        xs.push_back ((double) i * frameSec);
        ys.push_back (envDb[i]);
    }
    if (xs.size() < 2)
    {
        // Decayed past the span within one or two frames: report a very fast decay.
        return -spanDb / frameSec;
    }
    double mx = 0, my = 0;
    for (size_t i = 0; i < xs.size(); ++i) { mx += xs[i]; my += ys[i]; }
    mx /= (double) xs.size();
    my /= (double) xs.size();
    double num = 0, den = 0;
    for (size_t i = 0; i < xs.size(); ++i)
    {
        num += (xs[i] - mx) * (ys[i] - my);
        den += (xs[i] - mx) * (xs[i] - mx);
    }
    return den > 0 ? num / den : 0.0;
}

inline double peakDb (const std::vector<double>& envDb, double frameSec, double t0, double t1)
{
    const size_t a = (size_t) (t0 / frameSec);
    const size_t b = std::min (envDb.size(), (size_t) (t1 / frameSec));
    double p = -200.0;
    for (size_t i = a; i < b; ++i)
        p = std::max (p, envDb[i]);
    return p;
}

inline double levelAtDb (const std::vector<double>& envDb, double frameSec, double t)
{
    const size_t i = std::min (envDb.size() - 1, (size_t) (t / frameSec));
    return envDb[i];
}

// Mean absolute dB difference between two band envelopes, over frames where
// the reference is within `rangeDb` of its maximum (ignores the noise floor).
inline double meanAbsEnvelopeErrorDb (const std::vector<double>& outDb, const std::vector<double>& refDb,
                                      double rangeDb = 50.0)
{
    const size_t n = std::min (outDb.size(), refDb.size());
    double mx = -200.0;
    for (size_t i = 0; i < n; ++i)
        mx = std::max (mx, refDb[i]);
    double err = 0.0;
    size_t cnt = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (refDb[i] < mx - rangeDb && outDb[i] < mx - rangeDb)
            continue;
        err += std::fabs (std::max (outDb[i], mx - rangeDb) - std::max (refDb[i], mx - rangeDb));
        ++cnt;
    }
    return cnt ? err / (double) cnt : 0.0;
}

} // namespace analysis
