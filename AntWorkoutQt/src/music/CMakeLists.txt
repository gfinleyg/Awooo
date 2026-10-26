# awo_music: Musical Cadence Mode's music side, no screens (PLAN.md §3.3): picking songs, the tempo analysis (the
# Python app's, numpy's order and all), its cache, the ride built from it, and the player
add_library(awo_music STATIC
    AnalysisCache.cpp
    AnalysisCache.hpp
    CadenceWorkout.cpp
    CadenceWorkout.hpp
    MusicPlayer.cpp
    MusicPlayer.hpp
    NumpyLike.cpp
    NumpyLike.hpp
    Playlist.cpp
    Playlist.hpp
    TempoAnalysis.cpp
    TempoAnalysis.hpp
)
target_include_directories(awo_music PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(awo_music PUBLIC Qt6::Core awo_core PRIVATE miniaudio pocketfft)
awo_target_defaults(awo_music)
