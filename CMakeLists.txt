cmake_minimum_required(VERSION 3.22)

project(OrchPiano VERSION 0.1.0)

# Path to your JUCE folder.
add_subdirectory("C:/JUCE/JUCE" JUCE)

juce_add_plugin(OrchPiano
    COMPANY_NAME "JohnPascu"
    PLUGIN_MANUFACTURER_CODE Jpas
    PLUGIN_CODE Ocpn

    FORMATS VST3

    PRODUCT_NAME "OrchPiano"
    PLUGIN_NAME "OrchPiano"
    BUNDLE_ID "com.johnpascu.OrchPiano"

    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_REPLACES_VST2 FALSE

    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(OrchPiano)

target_sources(OrchPiano
    PRIVATE
        Source/OrchPianoProcessor.cpp
        Source/OrchPianoEditor.cpp
        Source/OrchPianoReductionLogic.cpp
)

target_compile_definitions(OrchPiano
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(OrchPiano
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_gui_extra

    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if (TARGET OrchPiano_VST3)
    target_compile_definitions(OrchPiano_VST3
        PRIVATE
            JUCE_VST3_CAN_REPLACE_VST2=0
    )
endif()

# Developer-only verification target for the pure reduction / voicing logic.
juce_add_console_app(OrchPianoReductionLogicCheck
    PRODUCT_NAME "OrchPianoReductionLogicCheck"
)

juce_generate_juce_header(OrchPianoReductionLogicCheck)

target_sources(OrchPianoReductionLogicCheck
    PRIVATE
        Tools/OrchPianoReductionLogicCheck.cpp
        Source/OrchPianoReductionLogic.cpp
)

target_link_libraries(OrchPianoReductionLogicCheck
    PRIVATE
        juce::juce_core

    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

target_include_directories(OrchPianoReductionLogicCheck
    PRIVATE
        Source
)
