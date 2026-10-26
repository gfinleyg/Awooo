# Command line tools over awo_core
qt_add_executable(awo-verifier awo_verifier.cpp)
target_link_libraries(awo-verifier PRIVATE awo_core)
awo_target_defaults(awo-verifier)

qt_add_executable(zwo2awo zwo2awo.cpp)
target_link_libraries(zwo2awo PRIVATE awo_core)
awo_target_defaults(zwo2awo)
