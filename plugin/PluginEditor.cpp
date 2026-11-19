#include "PluginEditor.h"

namespace Colours
{
const juce::Colour background { 0xff1c1a18 };
const juce::Colour panel { 0xff26231f };
const juce::Colour text { 0xffe9e4dc };
const juce::Colour dim { 0xff9a9289 };
const juce::Colour accent { 0xffd98c3f }; // warm "rubber" amber
const juce::Colour track { 0xff3a352f };
} // namespace Colours

// ---------------------------------------------------------------------------

RubberBridgeEditor::LookAndFeel::LookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, Colours::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, Colours::text);
    setColour (juce::ComboBox::backgroundColourId, Colours::track);
    setColour (juce::ComboBox::textColourId, Colours::text);
    setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::arrowColourId, Colours::accent);
    setColour (juce::PopupMenu::backgroundColourId, Colours::panel);
    setColour (juce::PopupMenu::textColourId, Colours::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::accent);
    setColour (juce::TooltipWindow::backgroundColourId, Colours::panel);
    setColour (juce::TooltipWindow::textColourId, Colours::text);
}

void RubberBridgeEditor::LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                                        float start, float end, juce::Slider&)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float lineW = juce::jmax (3.0f, radius * 0.12f);
    const float arcR = radius - lineW;
    const float angle = start + pos * (end - start);

    juce::Path bg;
    bg.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, start, end, true);
    g.setColour (Colours::track);
    g.strokePath (bg, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path fg;
    fg.addCentredArc (centre.x, centre.y, arcR, arcR, 0.0f, start, angle, true);
    g.setColour (Colours::accent);
    g.strokePath (fg, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float knobR = arcR - lineW * 1.4f;
    g.setColour (Colours::panel.brighter (0.15f));
    g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);
    juce::Path pointer;
    pointer.addRoundedRectangle (-lineW * 0.4f, -knobR, lineW * 0.8f, knobR * 0.5f, lineW * 0.4f);
    g.setColour (Colours::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
}

// ---------------------------------------------------------------------------

RubberBridgeEditor::RubberBridgeEditor (RubberBridgeProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setLookAndFeel (&lnf);

    setupKnob (mute, ParamIDs::mute, "Mute", "How much the rubber bridge damps the strings.");
    setupKnob (damping, ParamIDs::damping, "Damping",
               "How much faster the highs die than the lows. Higher = darker, more 'thunk'.");
    setupKnob (attack, ParamIDs::attack, "Attack",
               "Pick snap. 50% = physical. Lower = softer, darker plucks. Higher = more pick click.");
    setupKnob (thump, ParamIDs::thump, "Thump", "Low-mid body knock on each pluck.");
    setupKnob (retrigger, ParamIDs::retrigger, "Retrigger",
               "How fully each pluck re-opens the low strings. High for riffs and strumming, "
               "low for fingerstyle (melody over a ringing bass).");
    setupKnob (sensitivity, ParamIDs::sensitivity, "Sensitivity",
               "Pluck detection. Raise if soft re-picks get swallowed; lower if ringing chords re-open.");
    setupKnob (mix, ParamIDs::mix, "Mix", "Blend with the unmuted guitar (phase coherent).");
    setupKnob (output, ParamIDs::output, "Output", "Output level.");

    instrument.addItemList ({ "Electric", "Acoustic", "Baritone" }, 1);
    instrument.setTooltip ("Sets the band split and detector timing for the instrument's range.");
    addAndMakeVisible (instrument);
    instrumentAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.getState(), ParamIDs::instrument, instrument);
    instrumentLabel.setText ("Instrument", juce::dontSendNotification);
    instrumentLabel.setColour (juce::Label::textColourId, Colours::dim);
    instrumentLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (instrumentLabel);

    meterDb.fill (0.0f);
    setResizable (true, true);
    setResizeLimits (640, 380, 1280, 760);
    getConstrainer()->setFixedAspectRatio (720.0 / 420.0);
    setSize (720, 420);
    startTimerHz (30);
}

RubberBridgeEditor::~RubberBridgeEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void RubberBridgeEditor::setupKnob (Knob& k, const char* paramId, const juce::String& name, const juce::String& tooltip)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 18);
    k.slider.setTooltip (tooltip);
    addAndMakeVisible (k.slider);
    k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.getState(), paramId, k.slider);

    k.label.setText (name, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    addAndMakeVisible (k.label);
}

void RubberBridgeEditor::timerCallback()
{
    for (int k = 0; k < rb::kNumBands; ++k)
    {
        const float target = juce::jlimit (-60.0f, 0.0f, processor.getBandGainDb (k));
        // Fast fall, slower recovery so plucks are visible.
        meterDb[(size_t) k] = target < meterDb[(size_t) k] ? target : meterDb[(size_t) k] + 0.25f * (target - meterDb[(size_t) k]);
    }
    repaint (meterArea);
}

void RubberBridgeEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
    const float s = (float) getWidth() / 720.0f;

    g.setColour (Colours::text);
    g.setFont (juce::Font (juce::FontOptions (22.0f * s, juce::Font::bold)));
    g.drawText ("RUBBER BRIDGE", juce::Rectangle<int> (20, 12, 300, 32).toFloat() * s, juce::Justification::centredLeft);
    g.setColour (Colours::dim);
    g.setFont (juce::Font (juce::FontOptions (12.0f * s)));
    g.drawText ("soft bridge string damping  -  zero latency",
                juce::Rectangle<int> (22, 40, 360, 18).toFloat() * s, juce::Justification::centredLeft);

    // Meter panel
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (meterArea.toFloat().expanded (8.0f * s), 8.0f * s);
    const auto area = meterArea.toFloat();
    const float labelH = 16.0f * s;
    const auto bars = area.withTrimmedBottom (labelH).withTrimmedTop (labelH);
    g.setColour (Colours::dim);
    g.setFont (juce::Font (juce::FontOptions (11.0f * s)));
    g.drawText ("MUTING PER BAND", area.withHeight (labelH), juce::Justification::centred);

    const float barW = bars.getWidth() / (float) rb::kNumBands;
    for (int k = 0; k < rb::kNumBands; ++k)
    {
        const auto col = juce::Rectangle<float> (bars.getX() + barW * (float) k, bars.getY(), barW, bars.getHeight()).reduced (3.0f * s, 0.0f);
        g.setColour (Colours::track);
        g.fillRoundedRectangle (col, 2.0f * s);
        // Bar grows downward with the amount of muting (0 .. 60 dB).
        const float frac = juce::jlimit (0.0f, 1.0f, -meterDb[(size_t) k] / 60.0f);
        g.setColour (Colours::accent.withAlpha (0.85f));
        g.fillRoundedRectangle (col.withHeight (col.getHeight() * frac), 2.0f * s);

        const double hz = processor.getBandCentreHz (k);
        const juce::String txt = hz >= 1000.0 ? juce::String (hz / 1000.0, 1) + "k" : juce::String (juce::roundToInt (hz));
        g.setColour (Colours::dim);
        g.drawText (txt, juce::Rectangle<float> (col.getX() - 4.0f, area.getBottom() - labelH, col.getWidth() + 8.0f, labelH),
                    juce::Justification::centred);
    }
}

void RubberBridgeEditor::resized()
{
    const float s = (float) getWidth() / 720.0f;
    auto scaled = [s] (int x, int y, int w, int h) { return (juce::Rectangle<int> (x, y, w, h).toFloat() * s).toNearestInt(); };

    instrumentLabel.setBounds (scaled (400, 18, 110, 24));
    instrument.setBounds (scaled (520, 18, 180, 26));

    auto placeKnob = [&] (Knob& k, int x, int y, int size)
    {
        k.label.setBounds (scaled (x, y, size, 18));
        k.slider.setBounds (scaled (x, y + 18, size, size + 4));
        k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::roundToInt (72 * s), juce::roundToInt (18 * s));
        k.label.setFont (juce::Font (juce::FontOptions (14.0f * s, juce::Font::bold)));
    };

    // Main row: the four tone controls, Mute largest.
    placeKnob (mute, 20, 70, 130);
    placeKnob (damping, 160, 90, 100);
    placeKnob (attack, 270, 90, 100);
    placeKnob (thump, 380, 90, 100);

    // Second row: behaviour and output.
    placeKnob (retrigger, 20, 250, 86);
    placeKnob (sensitivity, 125, 250, 86);
    placeKnob (mix, 270, 250, 86);
    placeKnob (output, 375, 250, 86);

    meterArea = scaled (510, 78, 190, 320);
}
