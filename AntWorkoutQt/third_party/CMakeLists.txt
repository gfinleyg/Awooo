# Vendored libraries, each with its licence beside it
# miniaudio 0.11.25, the version the Python app's pyminiaudio 1.71 bundles (public domain or MIT-0): decoding mp3/flac/wav and playing them (PLAN.md D7)
add_library(miniaudio STATIC miniaudio/miniaudio.c miniaudio/miniaudio.h)
target_include_directories(miniaudio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/miniaudio")
# (third party: its own warnings stay its own)
target_compile_options(miniaudio PRIVATE /W0)

# pocketfft, numpy 1.26.4's C one (BSD-3): the tempo analysis' FFTs, the same as the Python app's numpy (PLAN.md D8)
add_library(pocketfft STATIC pocketfft/pocketfft.c pocketfft/pocketfft.h)
target_include_directories(pocketfft PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/pocketfft")
target_compile_options(pocketfft PRIVATE /W0)
