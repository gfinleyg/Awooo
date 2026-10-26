# AntWorkoutQt

AntWorkout rewritten in C++/Qt, alongside the Python app in `..\AntWorkout` (which stays as the reference). See
`PLAN.md` for the plan and where it's up to.

## Building

Needs Visual Studio 2022 with the C++ workload, Qt 6.7.0 for MSVC (`C:\Qt\6.7.0\msvc2019_64`, with Qt's own CMake
and Ninja in `C:\Qt\Tools`) and the ANT SDK at `..\..\antlib\ANT_LIB`, as for AntNative. From any PowerShell:

```
.\build_windows.ps1                        Release: build\Release, tests, then dist\AntWorkout\
.\build_windows.ps1 -Configuration Debug   build\Debug, dist\AntWorkout-Debug\
.\build_windows.ps1 -SkipTests
```

`dist\AntWorkout\` runs on its own: the exes, `antnative.dll`, Qt, the C++ runtime, and the fonts, sounds, images,
themes and workouts (copied from `..\AntWorkout`). Rebuilding never deletes anything there, and files in it that
are newer than the build's (settings, an edited theme or workout) are kept.

The C++ runtime has to be at least as new as the compiler that built the app (14.40 for VS 2022 17.10): AntNative's
`std::mutex` crashes with an older one, which is what PyQt6's bundled runtime did to the Python app.
`tests\tst_ant_sticks.cpp` checks the runtime that actually got loaded.

## Tools

Built beside the app (`build\Release\bin`, and `dist\AntWorkout\`):

| Tool               | What it does                                                                         |
|--------------------|--------------------------------------------------------------------------------------|
| `awo-verifier.exe` | Checks `.awo` files with the app's own reader, every bad line reported (files or folders, the current folder by default) |
| `zwo2awo.exe`      | Converts Zwift `.zwo` files to `.awo` files beside them (files or folders, the current folder by default) |

## Tests

`build_windows.ps1` runs them through ctest. To run them yourself, use `ctest --test-dir build\Release
--output-on-failure` (Qt's CMake: `C:\Qt\Tools\CMake_64\bin\ctest.exe`). They run headless (`QT_QPA_PLATFORM=offscreen`)
with Qt's bin folder put on PATH by ctest, each in its own empty working folder.

Most of them also compare against `tests\golden\*.json`: the Python app's own results for the same inputs (every
shipped workout, every theme's colors, grading scenarios...). After a deliberate change to the Python app's results,
write them again with `py -3.10 tests\golden\make_golden.py` (the Python that has AntWorkout's requirements and
requirements-test installed). Deliberate differences of the C++ app are listed in `CHANGES.md`.

### Widgets side by side with the Python app

The custom painted widgets (gauges, spirals, graphs...) are checked by eye and by pixel against the Python app's:
`tests\parity\render_python.py` renders the Python widgets, set up as the Python screens set them up and fed fixed
made-up readings, and `render_widgets.exe` (built with the tests, in `build\Release\bin`) renders the C++ ones from the
same setup. `tests\parity\compare.py` counts the differing pixels and writes each pair with its differences marked:

```
py -3.10 tests\parity\render_python.py %TEMP%\py
build\Release\bin\render_widgets.exe %TEMP%\cpp
py -3.10 tests\parity\compare.py %TEMP%\py %TEMP%\cpp %TEMP%\diff
```

Both run in `..\AntWorkout` (its `defaultTheme.json`) on the real Windows platform, not offscreen, so the fonts render
as they do on screen. Run `render_widgets.exe` with Qt's bin folder on PATH.

### Hardware tests with SimulANT+

`hwtests\simulant_test.py` is `..\AntWorkout\SimulantTests\run_simulant_test.py` with the C++ app riding: it
installs the same SimulANT+ scripts and follows the same timeline, but the ride is `simulant_ride.exe` (the app's real
ANT bridge, readings pipeline and workout screen in Free Mode), and the Python harness's own checks judge what it
recorded. Set up SimulANT+ as `..\AntWorkout\SimulantTests\README.md` says (its `simulant_automation` helpers work
here too), then:

```
py -3.10 hwtests\simulant_test.py trainer
py -3.10 hwtests\simulant_test.py sensors
```

Each run gets a folder in `..\AntWorkout\SimulantTests\run` (named `..._qt`) with `report.txt`, `app_samples.csv`,
the scripts' logs and the app's own log. `ant_console.exe` (in `dist\AntWorkout`) prints the readings of whatever
sensors it's told to wait for, for a quick look without the timeline.
