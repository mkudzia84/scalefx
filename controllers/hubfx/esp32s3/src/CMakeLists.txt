# Custom src/CMakeLists.txt — owned by us (gitignore lifted for this
# file).  pioarduino normally auto-generates this with just the sources
# glob, but we need to additionally declare component-manager
# dependencies that the firmware uses directly (esp-dsp for the SIMD
# mix kernel — Phase 5 of feature/idf-component-build).
#
# Format is the standard ESP-IDF `idf_component_register(...)`.
# pioarduino's espidf.py only writes a default here when missing (see
# HandleArduinoIDFsettings → create_default_project_files in the
# platform's espidf.py), so this file persists across rebuilds once
# committed.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES
        # Hand-tuned Xtensa LX7 SIMD for the int16 audio mix kernel.
        # Source lives in managed_components/espressif__esp-dsp/ (pulled
        # transitively by the framework); this REQUIRES is what actually
        # links the .a into the firmware.
        espressif__esp-dsp
)
