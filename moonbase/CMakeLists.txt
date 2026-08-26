# MoonBase: the second boot image for 4 MB boards.
#
# A board with one application slot cannot rewrite the partition it is executing from, so the
# install has to run from somewhere else. MoonBase is that somewhere: a small, rarely changing
# firmware in the `factory` partition that owns the device while the application is being replaced.
# Installing firmware is its first job, not its definition; see the plan for what may join it.
#
# It is a SEPARATE ESP-IDF project, and it shares no code with the application. Reusing the app's
# platform layer was measured at 788 KB with an empty app_main, because those sources drag RMT,
# I2S, PSRAM and JIT support with them. Written directly against ESP-IDF the same job measures
# ~590 KB. The duplication is the deliberate price of an image that must stay small and, once
# working, hardly change.
#
# The project name is load-bearing: `projectMM.bin` is matched by BASENAME in release.yml,
# flash_esp32.py, preview_installer.py and generate_manifest.py, and unknown basenames are skipped
# with only a warning, so MoonBase must not answer to it.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(projectMM-moonbase)
