// Synthetic evaluation: renders each test scenario with an ordinary bridge
// ("dry") and with ideal rubber-bridge physics ("ref"), runs dry through the
// plugin DSP ("out") and reports how close out is to ref.
//
//   rb_eval [outdir] [--mute x] [--damping x] [--attack x] [--thump x] [--retrigger x] [--sr 48000]
//
// With outdir, writes <scenario>_dry.wav, _ref.wav and _out.wav for listening.

#include "../dsp/RubberBridge.h"
#include "Analysis.h"
#include "GuitarSynth.h"
#include "WavIO.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main (int argc, char** argv)
{
    std::string outDir;
    rb::Params p;
    p.thump = 0.0f; // the reference has no thump
    double fs = 48000.0;

    int i = 1;
    if (argc > 1 && argv[1][0] != '-')
        outDir = argv[i++];
    for (; i + 1 < argc; i += 2)
    {
        const std::string k = argv[i];
        const double v = std::atof (argv[i + 1]);
        if (k == "--mute") p.mute = (float) v;
        else if (k == "--damping") p.damping = (float) v;
        else if (k == "--attack") p.attack = (float) v;
        else if (k == "--thump") p.thump = (float) v;
        else if (k == "--retrigger") p.retrigger = (float) v;
        else if (k == "--sr") fs = v;
    }

    // The physical target the plugin is trying to hit (same law as the DSP).
    rbsynth::Damping target;
    target.ratePer1k = rb::RubberBridge::bridgeRateAt1k (p.mute);
    target.alpha = rb::RubberBridge::tiltExponent (p.damping);

    std::printf ("mute=%.2f damping=%.2f attack=%.2f  (bridge rate %.1f Np/s @1k, alpha %.2f)\n\n",
                 p.mute, p.damping, p.attack, target.ratePer1k, target.alpha);
    std::printf ("%-24s %8s | %s\n", "scenario", "meanErr", "per-band mean |out-ref| dB   (dry-ref for scale)");

    double total = 0.0, totalDry = 0.0;
    int count = 0;
    for (const auto& sc : rbsynth::makeScenarios (fs))
    {
        const auto dry = rbsynth::render (sc.notes, sc.duration, sc.config, {});
        const auto ref = rbsynth::render (sc.notes, sc.duration, sc.config, target);

        auto out = dry;
        rb::RubberBridge dsp;
        rb::Params pp = p;
        pp.instrument = (rb::Instrument) sc.instrument;
        dsp.prepare (fs, 1);
        dsp.setParams (pp);
        dsp.reset();
        for (size_t pos = 0; pos < out.size(); pos += 64)
        {
            float* ptr = out.data() + pos;
            dsp.process (&ptr, 1, (int) std::min<size_t> (64, out.size() - pos));
        }

        const auto bOut = analysis::splitBands (out, fs);
        const auto bRef = analysis::splitBands (ref, fs);
        const auto bDry = analysis::splitBands (dry, fs);
        double sum = 0.0, sumDry = 0.0;
        std::string detail;
        for (int k = 0; k < analysis::kBands; ++k)
        {
            const auto eo = analysis::envelopeDb (bOut[(size_t) k], fs);
            const auto er = analysis::envelopeDb (bRef[(size_t) k], fs);
            const auto ed = analysis::envelopeDb (bDry[(size_t) k], fs);
            const double e = analysis::meanAbsEnvelopeErrorDb (eo, er);
            const double ed0 = analysis::meanAbsEnvelopeErrorDb (ed, er);
            sum += e;
            sumDry += ed0;
            char buf[64];
            std::snprintf (buf, sizeof buf, " %5.1f(%4.1f)", e, ed0);
            detail += buf;
        }
        const double mean = sum / analysis::kBands;
        total += mean;
        totalDry += sumDry / analysis::kBands;
        ++count;
        std::printf ("%-24s %8.2f |%s\n", sc.name.c_str(), mean, detail.c_str());

        if (! outDir.empty())
        {
            wav::writeMono (outDir + "/" + sc.name + "_dry.wav", dry, fs);
            wav::writeMono (outDir + "/" + sc.name + "_ref.wav", ref, fs);
            wav::writeMono (outDir + "/" + sc.name + "_out.wav", out, fs);
        }
    }
    std::printf ("\noverall mean envelope error: %.2f dB (unprocessed guitar: %.2f dB)\n", total / count, totalDry / count);
    return 0;
}
