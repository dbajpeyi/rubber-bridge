// Offline renderer: process a WAV file through the Rubber Bridge DSP.
//
//   rb_render in.wav out.wav [--mute 0.55] [--damping 0.5] [--attack 0.6]
//             [--thump 0.3] [--retrigger 0.8] [--sensitivity 0.5] [--mix 1] [--output 0] [--instrument electric|acoustic|baritone]
//             [--block 64]
//
// Parameters are 0..1 (output in dB). Output is 32-bit float WAV.

#include "../dsp/RubberBridge.h"
#include "WavIO.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static rb::Instrument parseInstrument (const std::string& s)
{
    if (s == "acoustic") return rb::Instrument::Acoustic;
    if (s == "baritone") return rb::Instrument::Baritone;
    return rb::Instrument::Electric;
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: %s in.wav out.wav [--mute x] [--damping x] [--attack x] [--thump x] "
                              "[--retrigger x] [--sensitivity x] [--mix x] [--output dB] [--instrument electric|acoustic|baritone] [--block n]\n",
                      argv[0]);
        return 1;
    }

    rb::Params p;
    int block = 64;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        const std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--mute") p.mute = std::strtof (v, nullptr);
        else if (k == "--damping") p.damping = std::strtof (v, nullptr);
        else if (k == "--attack") p.attack = std::strtof (v, nullptr);
        else if (k == "--thump") p.thump = std::strtof (v, nullptr);
        else if (k == "--retrigger") p.retrigger = std::strtof (v, nullptr);
        else if (k == "--sensitivity") p.sensitivity = std::strtof (v, nullptr);
        else if (k == "--mix") p.mix = std::strtof (v, nullptr);
        else if (k == "--output") p.outputDb = std::strtof (v, nullptr);
        else if (k == "--instrument") p.instrument = parseInstrument (v);
        else if (k == "--block") block = std::max (1, std::atoi (v));
        else { std::fprintf (stderr, "unknown option %s\n", k.c_str()); return 1; }
    }

    try
    {
        wav::Audio a = wav::read (argv[1]);
        if (a.channels.size() > 2)
            a.channels.resize (2);

        rb::RubberBridge dsp;
        dsp.prepare (a.sampleRate, (int) a.channels.size());
        dsp.setParams (p);
        dsp.reset();

        const size_t n = a.numSamples();
        float* ptrs[2] = { nullptr, nullptr };
        for (size_t pos = 0; pos < n; pos += (size_t) block)
        {
            const int len = (int) std::min<size_t> ((size_t) block, n - pos);
            for (size_t c = 0; c < a.channels.size(); ++c)
                ptrs[c] = a.channels[c].data() + pos;
            dsp.process (ptrs, (int) a.channels.size(), len);
        }
        wav::write (argv[2], a);
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
