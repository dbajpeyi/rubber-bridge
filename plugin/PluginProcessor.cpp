#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
juce::String percentText (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; }
juce::String dbText (float v, int) { return juce::String (v, 1) + " dB"; }

std::unique_ptr<juce::AudioParameterFloat> percentParam (const char* id, const char* name, float def)
{
    return std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), def,
        juce::AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (percentText));
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout RubberBridgeProcessor::createLayout()
{
    const rb::Params d; // defaults live in the DSP core
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (percentParam (ParamIDs::mute, "Mute", d.mute * 100.0f));
    layout.add (percentParam (ParamIDs::damping, "Damping", d.damping * 100.0f));
    layout.add (percentParam (ParamIDs::attack, "Attack", d.attack * 100.0f));
    layout.add (percentParam (ParamIDs::thump, "Thump", d.thump * 100.0f));
    layout.add (percentParam (ParamIDs::retrigger, "Retrigger", d.retrigger * 100.0f));
    layout.add (percentParam (ParamIDs::sensitivity, "Sensitivity", d.sensitivity * 100.0f));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { ParamIDs::instrument, 1 }, "Instrument",
        juce::StringArray { "Electric", "Acoustic", "Baritone" }, (int) d.instrument));
    layout.add (percentParam (ParamIDs::mix, "Mix", d.mix * 100.0f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::output, 1 }, "Output", juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f),
        d.outputDb, juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
    return layout;
}

RubberBridgeProcessor::RubberBridgeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "RubberBridge", createLayout())
{
    pMute = state.getRawParameterValue (ParamIDs::mute);
    pDamping = state.getRawParameterValue (ParamIDs::damping);
    pAttack = state.getRawParameterValue (ParamIDs::attack);
    pThump = state.getRawParameterValue (ParamIDs::thump);
    pRetrigger = state.getRawParameterValue (ParamIDs::retrigger);
    pSensitivity = state.getRawParameterValue (ParamIDs::sensitivity);
    pInstrument = state.getRawParameterValue (ParamIDs::instrument);
    pMix = state.getRawParameterValue (ParamIDs::mix);
    pOutput = state.getRawParameterValue (ParamIDs::output);
}

bool RubberBridgeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    // mono->mono, stereo->stereo, and mono->stereo (guitar DI on a stereo track)
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

rb::Params RubberBridgeProcessor::readParams() const
{
    rb::Params p;
    p.mute = pMute->load() * 0.01f;
    p.damping = pDamping->load() * 0.01f;
    p.attack = pAttack->load() * 0.01f;
    p.thump = pThump->load() * 0.01f;
    p.retrigger = pRetrigger->load() * 0.01f;
    p.sensitivity = pSensitivity->load() * 0.01f;
    p.instrument = (rb::Instrument) juce::jlimit (0, 2, juce::roundToInt (pInstrument->load()));
    p.mix = pMix->load() * 0.01f;
    p.outputDb = pOutput->load();
    return p;
}

void RubberBridgeProcessor::prepareToPlay (double sampleRate, int)
{
    dsp.setParams (readParams());
    dsp.prepare (sampleRate, juce::jmax (1, getTotalNumOutputChannels()));
    setLatencySamples (0);
}

void RubberBridgeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int n = buffer.getNumSamples();

    // Mono in, stereo out: process the mono signal and copy it to both sides.
    const bool monoToStereo = numIn == 1 && numOut == 2;
    for (int ch = numIn; ch < numOut; ++ch)
        if (! monoToStereo)
            buffer.clear (ch, 0, n);

    dsp.setParams (readParams());
    const int procChannels = juce::jmin (monoToStereo ? 1 : numOut, 2);
    dsp.process (buffer.getArrayOfWritePointers(), procChannels, n);

    if (monoToStereo)
        buffer.copyFrom (1, 0, buffer, 0, 0, n);
}

juce::AudioProcessorEditor* RubberBridgeProcessor::createEditor()
{
    return new RubberBridgeEditor (*this);
}

void RubberBridgeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = state.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void RubberBridgeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (state.state.getType()))
            state.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RubberBridgeProcessor();
}
