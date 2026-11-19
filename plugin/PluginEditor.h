#pragma once

#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

class RubberBridgeEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit RubberBridgeEditor (RubberBridgeProcessor&);
    ~RubberBridgeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    class LookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end,
                               juce::Slider&) override;
    };

    void timerCallback() override;
    void setupKnob (Knob&, const char* paramId, const juce::String& name, const juce::String& tooltip);

    RubberBridgeProcessor& processor;
    LookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 600 };

    Knob mute, damping, attack, thump, retrigger, sensitivity, mix, output;
    juce::ComboBox instrument;
    juce::Label instrumentLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> instrumentAttachment;

    std::array<float, rb::kNumBands> meterDb {};
    juce::Rectangle<int> meterArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RubberBridgeEditor)
};
