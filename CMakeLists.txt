cmake_minimum_required(VERSION 3.22)
project(HardInsert VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# JUCE 8.0.15 ships the ASIO SDK headers (Steinberg, dual licensed: ASIO License / GPLv3).
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# CLAP support for JUCE plugins.
FetchContent_Declare(
    clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG 3837f4a6ed2ab0ba46aba69ae5b7041c7c64ce74
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(clap-juce-extensions)

juce_add_plugin(HardInsert
    COMPANY_NAME "belovw"
    BUNDLE_ID "com.belovw.hardinsert"
    PLUGIN_MANUFACTURER_CODE Blwv
    PLUGIN_CODE Hins
    PRODUCT_NAME "Hard-Insert"
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES "Fx" "Dynamics"
)

clap_juce_extensions_plugin(TARGET HardInsert
    CLAP_ID "com.belovw.hardinsert"
    CLAP_FEATURES audio-effect compressor utility)

target_sources(HardInsert PRIVATE
    Source/HardwareIO.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_definitions(HardInsert PUBLIC
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(HardInsert
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
)

# Timing simulation of the hardware loop (DAW thread <-> device thread).
option(HARDINSERT_BUILD_TESTS "Build the loop timing test" ON)

if (HARDINSERT_BUILD_TESTS)
    juce_add_console_app(HardInsertPortTest PRODUCT_NAME "HardInsertPortTest")

    target_sources(HardInsertPortTest PRIVATE
        tests/port_test.cpp
        Source/HardwareIO.cpp
    )

    target_compile_definitions(HardInsertPortTest PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )

    target_link_libraries(HardInsertPortTest
        PRIVATE
            juce::juce_core
            juce::juce_events
            juce::juce_data_structures
            juce::juce_audio_basics
            juce::juce_audio_devices
        PUBLIC
            juce::juce_recommended_config_flags
    )
endif()
