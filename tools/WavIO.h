#pragma once
// Minimal WAV reader/writer for the offline tools (PCM 16/24/32 and float32/64).

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace wav
{

struct Audio
{
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> channels; // [ch][sample]

    size_t numSamples() const { return channels.empty() ? 0 : channels[0].size(); }
};

namespace detail
{
inline uint32_t rd32 (const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24); }
inline uint16_t rd16 (const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }
inline void wr32 (std::ofstream& f, uint32_t v) { f.write (reinterpret_cast<const char*> (&v), 4); }
inline void wr16 (std::ofstream& f, uint16_t v) { f.write (reinterpret_cast<const char*> (&v), 2); }
} // namespace detail

inline Audio read (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    if (! f)
        throw std::runtime_error ("cannot open " + path);
    std::vector<uint8_t> data ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp (data.data(), "RIFF", 4) != 0 || std::memcmp (data.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error ("not a WAV file: " + path);

    uint16_t format = 0, numCh = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmBytes = 0;

    size_t pos = 12;
    while (pos + 8 <= data.size())
    {
        const uint32_t size = detail::rd32 (&data[pos + 4]);
        const uint8_t* body = &data[pos + 8];
        const size_t avail = std::min<size_t> (size, data.size() - pos - 8);
        if (std::memcmp (&data[pos], "fmt ", 4) == 0 && avail >= 16)
        {
            format = detail::rd16 (body);
            numCh = detail::rd16 (body + 2);
            rate = detail::rd32 (body + 4);
            bits = detail::rd16 (body + 14);
            if (format == 0xFFFE && avail >= 26)
                format = detail::rd16 (body + 24); // WAVE_FORMAT_EXTENSIBLE sub-format
        }
        else if (std::memcmp (&data[pos], "data", 4) == 0)
        {
            pcm = body;
            pcmBytes = avail;
        }
        pos += 8 + size + (size & 1);
    }
    if (! pcm || numCh == 0)
        throw std::runtime_error ("missing fmt/data chunk: " + path);

    const size_t bytesPerSample = bits / 8;
    const size_t frames = pcmBytes / (bytesPerSample * numCh);
    Audio a;
    a.sampleRate = rate;
    a.channels.assign (numCh, std::vector<float> (frames));

    for (size_t i = 0; i < frames; ++i)
        for (size_t c = 0; c < numCh; ++c)
        {
            const uint8_t* p = pcm + (i * numCh + c) * bytesPerSample;
            float v = 0.0f;
            if (format == 1 && bits == 16)
                v = (float) (int16_t) detail::rd16 (p) / 32768.0f;
            else if (format == 1 && bits == 24)
            {
                int32_t s = (int32_t) (p[0] | (p[1] << 8) | (p[2] << 16));
                if (s & 0x800000) s |= ~0xFFFFFF;
                v = (float) s / 8388608.0f;
            }
            else if (format == 1 && bits == 32)
                v = (float) ((double) (int32_t) detail::rd32 (p) / 2147483648.0);
            else if (format == 3 && bits == 32)
                std::memcpy (&v, p, 4);
            else if (format == 3 && bits == 64)
            {
                double d;
                std::memcpy (&d, p, 8);
                v = (float) d;
            }
            else
                throw std::runtime_error ("unsupported WAV format in " + path);
            a.channels[c][i] = v;
        }
    return a;
}

// Writes 32-bit float WAV.
inline void write (const std::string& path, const Audio& a)
{
    std::ofstream f (path, std::ios::binary);
    if (! f)
        throw std::runtime_error ("cannot write " + path);
    const uint16_t numCh = (uint16_t) a.channels.size();
    const uint32_t frames = (uint32_t) a.numSamples();
    const uint32_t dataBytes = frames * numCh * 4;

    f.write ("RIFF", 4);
    detail::wr32 (f, 36 + dataBytes);
    f.write ("WAVE", 4);
    f.write ("fmt ", 4);
    detail::wr32 (f, 16);
    detail::wr16 (f, 3);
    detail::wr16 (f, numCh);
    detail::wr32 (f, (uint32_t) a.sampleRate);
    detail::wr32 (f, (uint32_t) a.sampleRate * numCh * 4);
    detail::wr16 (f, (uint16_t) (numCh * 4));
    detail::wr16 (f, 32);
    f.write ("data", 4);
    detail::wr32 (f, dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < numCh; ++c)
            f.write (reinterpret_cast<const char*> (&a.channels[c][i]), 4);
}

inline void writeMono (const std::string& path, const std::vector<float>& x, double fs)
{
    Audio a;
    a.sampleRate = fs;
    a.channels.push_back (x);
    write (path, a);
}

} // namespace wav
