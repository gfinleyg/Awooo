# Command line tools over awo_core
qt_add_executable(awo-verifier awo_verifier.cpp)
target_link_libraries(awo-verifier PRIVATE awo_core)
awo_target_defaults(awo-verifier)

qt_add_executable(zwo2awo zwo2awo.cpp)
target_link_libraries(zwo2awo PRIVATE awo_core)
awo_target_defaults(zwo2awo)

# ColorPicker.exe: the standalone RGBA color picker (ColorPicker.py)
qt_add_executable(ColorPicker WIN32 ColorPicker.cpp)
target_link_libraries(ColorPicker PRIVATE awo_ui)
awo_target_defaults(ColorPicker)

# FtpColorsEditor.exe: LayoutViewer's FTP Colors tab and mockup on their own
qt_add_executable(FtpColorsEditor WIN32 FtpColorsEditor.cpp)
target_link_libraries(FtpColorsEditor PRIVATE awo_ui)
awo_target_defaults(FtpColorsEditor)
add_dependencies(FtpColorsEditor awo_assets)
