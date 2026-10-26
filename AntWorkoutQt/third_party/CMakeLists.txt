# Vendored libraries, each with its licence beside it
# miniaudio 0.11.21 (public domain or MIT-0): decoding mp3/flac/wav and playing them (PLAN.md D7)
add_library(miniaudio STATIC miniaudio/miniaudio.c miniaudio/miniaudio.h)
target_include_directories(miniaudio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/miniaudio")
# (third party: its own warnings stay its own)
target_compile_options(miniaudio PRIVATE /W0)
