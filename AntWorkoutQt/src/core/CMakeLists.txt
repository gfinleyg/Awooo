# awo_core: the workouts, grading, session, settings and zone color math, with no screens (PLAN.md §3.1).
# Qt Gui only for QColor, so the colors are worked out exactly as the Python app's PyQt6 did.
add_library(awo_core STATIC
    AppState.hpp
    AwoParser.cpp
    AwoParser.hpp
    FtpColors.cpp
    FtpColors.hpp
    FtpTest.cpp
    FtpTest.hpp
    Logger.cpp
    Logger.hpp
    Performance.cpp
    Performance.hpp
    PowerCalibration.cpp
    PowerCalibration.hpp
    PyCompat.cpp
    PyCompat.hpp
    PyRandom.cpp
    PyRandom.hpp
    SavedSettings.cpp
    SavedSettings.hpp
    Session.cpp
    Session.hpp
    SimulatedRider.cpp
    SimulatedRider.hpp
    Units.cpp
    Units.hpp
    WorkoutData.cpp
    WorkoutData.hpp
    WorkoutEngine.cpp
    WorkoutEngine.hpp
    WorkoutGrading.cpp
    WorkoutGrading.hpp
    WorkoutHelpers.cpp
    WorkoutHelpers.hpp
    WorkoutTypes.hpp
    ZwoConvert.cpp
    ZwoConvert.hpp
)
target_include_directories(awo_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(awo_core PUBLIC Qt6::Core Qt6::Gui PRIVATE dbghelp)
awo_target_defaults(awo_core)
