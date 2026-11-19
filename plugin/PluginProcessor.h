#pragma once

#include "RubberBridge.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace ParamIDs
{
inline constexpr auto mute = "mute";
inline constexpr auto damping = "damping";
inline constexpr auto attack = "attack";
inline constexpr auto thump = "thump";
inline constexpr auto retrigger = "retrigger";
inline constexpr auto sensitivity = "sensitivity";
inline constexpr auto instrument = "instrument";
inline constexpr auto mix = "mix";
inline constexpr auto output = "output";
} // namespace ParamIDs

class RubberBridgeProcessor final : public juce::AudioProcessor
{
public:
    RubberBridgeProcessor();
    ~RubberBridgeProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getState() { return state; }

    // UI meters (safe from the message thread).
    float getBandGainDb (int band) const { return dsp.getBandGainDb (band); }
    double getBandCentreHz (int band) const { return dsp.getBandCentreHz (band); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    rb::Params readParams() const;

    juce::AudioProcessorValueTreeState state;
    rb::RubberBridge dsp;

    std::atomic<float>* pMute = nullptr;
    std::atomic<float>* pDamping = nullptr;
    std::atomic<float>* pAttack = nullptr;
    std::atomic<float>* pThump = nullptr;
    std::atomic<float>* pRetrigger = nullptr;
    std::atomic<float>* pSensitivity = nullptr;
    std::atomic<float>* pInstrument = nullptr;
    std::atomic<float>* pMix = nullptr;
    std::atomic<float>* pOutput = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RubberBridgeProcessor)
};
