cmake_minimum_required(VERSION 3.22)

project(RubberBridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(RB_BUILD_PLUGIN "Build the JUCE plugin (AU/VST3/Standalone)" ON)
option(RB_BUILD_TOOLS "Build offline tools and tests" ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

# ---- DSP core (header only, no dependencies) ---------------------------------
add_library(rb_dsp INTERFACE)
target_include_directories(rb_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dsp)

# ---- Offline tools & tests ---------------------------------------------------
if(RB_BUILD_TOOLS)
    add_executable(rb_render tools/rb_render.cpp)
    target_link_libraries(rb_render PRIVATE rb_dsp)

    add_executable(rb_eval tools/rb_eval.cpp)
    target_link_libraries(rb_eval PRIVATE rb_dsp)

    enable_testing()
    add_executable(rb_tests tests/test_dsp.cpp)
    target_link_libraries(rb_tests PRIVATE rb_dsp)
    add_test(NAME rb_tests COMMAND rb_tests)
endif()

# ---- Plugin ------------------------------------------------------------------
if(RB_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)

    set(RB_FORMATS VST3 Standalone)
    if(APPLE)
        list(APPEND RB_FORMATS AU)
    endif()

    juce_add_plugin(RubberBridge
        COMPANY_NAME "Rubber Bridge"
        BUNDLE_ID "com.rubberbridge.plugin"
        PLUGIN_MANUFACTURER_CODE Rbbr
        PLUGIN_CODE Rbg1
        FORMATS ${RB_FORMATS}
        PRODUCT_NAME "Rubber Bridge"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        AU_MAIN_TYPE kAudioUnitType_Effect
        VST3_CATEGORIES Fx Dynamics
        COPY_PLUGIN_AFTER_BUILD FALSE)

    target_sources(RubberBridge PRIVATE
        plugin/PluginProcessor.cpp
        plugin/PluginEditor.cpp)

    target_compile_definitions(RubberBridge PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(RubberBridge
        PRIVATE
            rb_dsp
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()
