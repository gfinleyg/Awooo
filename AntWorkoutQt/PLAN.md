# AntWorkout in C++/Qt: rewrite plan

Rewrite the PyQt6 app in `AntWorkout/` (about 20k lines of Python, 45 modules, about 400 unit tests) as a native
C++/Qt 6 app, keeping every feature, every file format and the look of every screen.

Nothing here is built yet. Each phase below ends with something that builds, runs and has passing tests.

---

## 1. Decisions

| # | Decision | Recommendation | Why |
|---|---|---|---|
| D1 | C or C++ | **C++20** for the app. The ANT layer is already C/C++ (AntNative). | Qt is a C++ framework, so there is no C option for the UI. MSVC 2022 supports C++20 fully. |
| D2 | Qt version | **Qt 6.7.0, msvc2019_64 kit** (already in `C:\Qt\6.7.0`) | The layouts and fonts were tuned under Qt 6.7, whose default Windows font engine is GDI. Qt 6.8 switched to DirectWrite, which changes text metrics and would break the text fitting. Upgrade later as its own task, if ever. |
| D3 | Where it lives | New folder `AntWorkoutQt/` beside `AntWorkout/`. **The Python app stays in the repo after switch-over** (decided). | Both apps keep working and read the same settings, workouts, themes and caches, so you can switch between them. The Python app stays the reference for behaviour. |
| D4 | How it talks to ANT | **AntNative's core, linked statically** (changed in Phase 2 from `antnative.hpp` over `antnative.dll`) | The C API polls messages without saying which sensor each came from, or whether it brought a new reading, and doesn't expose the raw counters the reading pipeline works from. The core gives the Python bridge's exact callback (message, sensor, changed) and the profiles' readings. Its `Transport` interface lets the tests run the real Manager and Recovery on a fake stick, as the Python tests did. Both are built by the same compiler in one CMake tree, so there's no ABI to keep apart, and the app no longer ships `antnative.dll`. |
| D5 | Build system | **CMake + Ninja** (both ship with Qt in `C:\Qt\Tools`), with a `build_windows.ps1` like AntNative's | AntNative already has a `CMakeLists.txt`. The app can `add_subdirectory(../AntNative)` and build one tree. |
| D6 | Graphs (replaces pyqtgraph) | **A small in-house plot library on QGraphicsView** (decided: option A in §4.4) | pyqtgraph is itself built on QGraphicsView, so the app's own graph items port line for line. Qwt stays the fallback if the Phase 4 spike goes badly. |
| D7 | Audio (decode, analysis input, playback, UI sounds) | **miniaudio** (one C header; the same library the Python app uses through pyminiaudio) | Qt Multimedia isn't installed, and the music player uses the audio device position as the workout clock, which miniaudio exposes directly. |
| D8 | FFT for tempo analysis | **pocketfft** (header-only C++, BSD) | It's the FFT numpy uses, so the analysis can match the Python results closely enough to share the cache. |
| D9 | Unit tests | **Qt Test** (installed), run headless with `QT_QPA_PLATFORM=offscreen` | No extra dependency, and it covers both logic and widget tests. |
| D10 | How faithful | **Improve along the way, as long as every feature still works the same for the rider** (decided) | Port module by module so each C++ file can still be checked against its Python original, but restructure, simplify and fix things as they come up. |

**The rule for improvements (D10):** a change is fine when the rider can still do everything they could before, with
the same files, and the tests for that feature still pass. Every change the rider can see (a look, a wording, a
behaviour) goes in `AntWorkoutQt/CHANGES.md` with the reason, so "functionality preserved" can be checked rather
than remembered.

Improvements already planned:
- Themes with explicit calls instead of monkeypatching `setStyleSheet`/`setText` (§4.2)
- No "import ant_native before PyQt6" DLL workaround, and no `os._exit` on shutdown
- Music analysis on a thread pool instead of worker processes
- One `.awo` parser, taken out of the setup screen, shared with the verifier
- The workout engine taken out of the HUD, and the reading pipeline out of the poller
- `AppState` globals turned into owned objects, step by step
- `QStackedWidget` screen switching instead of adding, removing and hiding screens
- Crash dumps in place of Python tracebacks

---

## 2. Target layout

```
AntWorkoutQt/
  CMakeLists.txt            top level: Qt, AntNative (add_subdirectory), third_party, targets below
  build_windows.ps1         configure + build + windeployqt + copy assets/DLLs into dist\
  third_party/              miniaudio.h, pocketfft_hdronly.hpp (vendored, with their licences)
  src/
    core/      awo_core     (static lib, QtCore only, no widgets): workouts, grading, session, settings, colors math
    music/     awo_music    (static lib): decode, tempo analysis, cache, workout builder, player
    ant/       awo_ant      (static lib, QtCore): bridge thread, reading pipeline (the poller's logic), fake
    plot/      awo_plot     (static lib): the pyqtgraph replacement
    ui/        awo_ui       (static lib, QtWidgets/QtSvg): theme, layout, widgets, screens, dialogs
    app/       AntWorkout.exe        main()
    viewer/    LayoutViewer.exe      the layout/theme editor
    tools/     awo-verifier.exe, zwo2awo.exe, FtpColorsEditor.exe, ColorPicker.exe
  tests/       one Qt Test exe per area (core, music, ant, ui, flow) + golden fixtures
  hwtests/     SimulANT+ harnesses (C++), reusing AntWorkout/SimulantTests/simulant_scripts as they are
  assets/      symlinked or copied at build time from AntWorkout/: fonts, sounds, svgs, themes, awoWorkouts
```

The app and LayoutViewer are thin `main()`s over the same libraries, so tests and the viewer drive the real screens,
just as the Python tests and LayoutViewer import the real modules today.

---

## 3. Module map: Python to C++

Line counts are the Python originals.

### 3.1 Core logic: `awo_core` (no widgets; ported first)

| Python | C++ | Notes |
|---|---|---|
| `WorkoutHelpers.py` (155) | `core/WorkoutHelpers` | Block text, targets, dropout handling, selected-device lines |
| `ClassWorkoutSetupWindow.process_workout_*`, `ramp_block`, `process_ramp`, message parsing (~400 lines inside the 2128-line setup screen) | `core/AwoParser` | **Pulled out of the screen** into a parser returning `Workout` (blocks, messages, title, duration) or an error with its line number |
| `awo-verifier.py` (269) | `core/AwoParser` validation + `tools/awo-verifier` | One parser for both. Today the verifier repeats the rules. |
| `ZwoConvert.py` (313), `zwo2awo.py` | `core/ZwoConvert` (QXmlStreamReader) + `tools/zwo2awo` | Keep the stray-closing-tag repair |
| `FtpTest.py` (159) | `core/FtpTest` | Test definitions, effort average, best rolling average |
| `WorkoutGrading.py` (296) | `core/WorkoutGrading` | `BlockGrader`, letter grades, rows, averages |
| `Session.py` (208) | `core/Session` | Rides in memory, ring history |
| `WorkoutData.py` (74) | `core/WorkoutData` | `SensorHistory` |
| `Units.py` (153) | `core/Units` | With the change notifications as a Qt signal |
| `SavedSettings.py` (98) | `core/SavedSettings` | **Same `savedSettingsANT.json`**, atomic write (QSaveFile), legacy migration |
| `PowerCalibration.py` (79) | `core/PowerCalibration` | Same keys (`"11 12345"`) inside the settings file |
| `FtpColors.py` (609) | `core/FtpColors` (color math: OKLCH, zones, fitted palettes) + `ui/` (brushes, pens) | Pure math split from the Qt brushes |
| `Performance.py` (66) | `core/Performance` | Gauge FPS setting |
| `ClassAntLogger.py` (187) | `core/Logger` | Same `antWorkoutLog.txt` format and rotation; `qInstallMessageHandler` plus a crash handler (SetUnhandledExceptionFilter + minidump) in place of the Python excepthooks |
| `AppState.py` (137) | `core/AppState` | One object instead of class attributes (see §4.1) |

### 3.2 ANT: `awo_ant`

| Python | C++ | Notes |
|---|---|---|
| `ClassAntDevices.py` (1041), `AntWorkoutBridge(QThread)` | `ant/AntBridge` (QObject on its own QThread) | Same signals (`deviceDiscovered`, `devicesConnected`, `devicesDisconnected`, `dataUpdated`, `statusMessage`, `errorOccurred`, `calibrationUpdate`, `batteryChanged`) and the same thread-safe request methods. Keeps the init retries (4 attempts, 1/2/4 s), stick hot-plug polling, the 5 s scan settle, calibration resends, trainer target resend/fast retry/release, battery change logging and the event logging filter. |
| `ClassAntDevicesPoller.py` (576) | `ant/ReadingPipeline` (logic) + HUD wiring | **Split from the widgets**: tick math (rollover, the 513/2305 quirk, 200 RPM/60 mph limits), hold-last-value smoothing, cadence/speed fallbacks to the power meter, the power-to-speed estimate, calibration. Emits plain readings, which the HUD shows. Testable without widgets. |
| `AntNative/python/ant/*` | not ported | The app uses `antnative::Manager` directly. The Python package's tests (manager/profiles/recovery, 73 tests) are covered by AntNative's own core tests. |

### 3.3 Music: `awo_music`

| Python | C++ | Notes |
|---|---|---|
| `MusicCadence.py` (898): picking, playlists, decode, onset envelope, tempo track, beat tracking, silences, cache, segments, RPM folding, workout builder | `music/*` split into `Playlist`, `Decoder` (miniaudio to mono 11025 Hz), `TempoAnalysis` (pocketfft), `AnalysisCache` (same `musicCadenceCacheANT.json`, CRC keys, `cache_version`), `CadenceWorkout` | Analysis runs on a `QThreadPool`/QtConcurrent pool, not processes. Port numpy line by line with the same window, hop, FFT sizes and median filters. |
| `MusicCadence.MusicPlayer` | `music/MusicPlayer` | miniaudio device, read-ahead, position from frames played less latency, as now |
| `SoundEffects.py` (129) | `ui/SoundEffects` | miniaudio, preloaded wavs, a button click filter like `_ButtonSoundFilter` |

### 3.4 UI foundation: `awo_ui`

| Python | C++ | Notes |
|---|---|---|
| `AppTheme.py` (742) | `ui/Theme` | **Redesigned, same theme files** (see §4.2) |
| `AppStyle.py` (33) | `ui/AppStyle` | The app stylesheet |
| `AppLayout.py` (509) | `ui/AppLayout` | Scaling from 1920x1152, the gap grid, text fitting, screen fit, symbol buttons, moving between monitors. Uses the same Qt APIs, so it ports directly. |
| `KeyShortcuts.py` (294) | `ui/KeyShortcuts` | Application event filter, Ctrl shows labels, tab cycling, list stepping |
| `ClassCustomTitleBar.py` (253) | `ui/CustomTitleBar` | Frameless drag, snapping, the gear menu |
| `ClassMainWindow.py` (232) | `ui/MainWindow` | Screen switching. Consider a `QStackedWidget` in place of add/remove/hide (same visible behaviour, fewer of the "two copies stacked" edge cases the Python works around). |
| `AntWorkout.py` (102) | `app/main.cpp` | Startup order: logger, QApplication, theme, shortcuts, sounds, saved and random theme, window. No DLL-order hack. |

### 3.5 Custom painted widgets

| Python | C++ | Notes |
|---|---|---|
| `ClassAnalogGauge.py` (1954) | `ui/widgets/AnalogGauge` | The biggest widget: layered pixmap caching, needle animation, value colour gradients, center rings, fitted value font. Port paint code 1:1. |
| `QSpiralProgressBar.py` (809), `ClassSpiralProgressBar.py` (51) | (not ported) | Never built by the app (Phase 4) |
| `ClassPowerSpiral.py` (492) | `ui/widgets/PowerSpiral` | Timeline binning, ribbon paths, cached layers |
| `ClassSmoothProgressBar.py` (50) | `ui/widgets/SmoothProgressBar` | |
| `ClassLongVerticalLabelSeries.py` (175) | `ui/widgets/LongVLabels` | Drop the Designer-style `Ui_Form` |
| `ClassGradeWidgets.py` (443) | `ui/widgets/GradeOverlay`, `GradeList` | |
| `ClassWorkoutGraph.py` (91) | `ui/widgets/WorkoutGraph` on `awo_plot` | |
| `ClassSolidColoredColumns.py` (252) | `plot/SolidColoredColumns` | |
| `ClassMaxStar.py` (335) | `ui/widgets/MaxStar`, `MaxBubble` | In the theme's colors, so in awo_ui |

### 3.6 Screens and dialogs

| Python | C++ | Notes |
|---|---|---|
| `ClassWorkoutSetupWindow.py` (2128) | `ui/screens/SetupScreen` | Scan, auto-select and connect, blinking, FTP/HR/wheel/weight/max-watts validators, modes, FTP test builder, music workout builder, workout preview. Parsing moves to `core/AwoParser`. |
| `ClassHUDWindow.py` (2464) | `ui/screens/HudScreen` + **`core/WorkoutEngine`** | **Split**: the engine (100 ms tick, block progression, ERG targets and the power drive cutoff, difficulty, pause/restart, messages, grading hooks, FTP test result, music clock) becomes a tested class with no widgets. The screen shows its state. |
| `ClassSynopsisWindow.py` (1549) | `ui/screens/SynopsisScreen` | Linked plots, strips, grade regions and popups, zoom, time edits, max markers and stars, FTP apply |
| `ClassSelectDevicesWindow.py` (270), `ClassConnectDevicesWindow.py` (145) | `ui/dialogs/*` | |
| `ClassWorkoutSelectWindow.py` (329) | `ui/dialogs/WorkoutSelectDialog` | Preview cache, random pick, ZWO conversion |
| `ClassMusicSelectWindow.py` (373) | `ui/dialogs/MusicSelectDialog` | Analysis progress from the pool |
| `ClassCalibratePowerWindow.py` (379) | `ui/dialogs/CalibratePowerDialog` | |
| `ClassThemeSelectWindow.py` (447) + `PreviewRide.py` (375) | `ui/dialogs/ThemeSelectDialog` + `ui/PreviewRide` | The live workout preview needs a fake bridge and a ride sandbox, as now |
| `ClassConfirm*Window.py`, `ClassShowLogsWindow.py`, `ClassMeasurementsWindow.py`, `ClassPerformanceWindow.py` | `ui/dialogs/*` | Small |

### 3.7 Dev tools

| Python | C++ |
|---|---|
| `LayoutViewer.py` (1532) | `viewer/LayoutViewer.exe` (decided): every screen without a stick, font/colour/theme editing, scenarios, screenshots |
| `FtpColorsEditor.py` (332) | `viewer/FtpColorsTab` + `FtpMockupWindow` (decided): the power zone colour editor and its mockup (zone colours on a mock power gauge, block labels and graph bars, for any FTP and power). Today it only runs as LayoutViewer's FTP Colors tab. It stays there, and also gets a small `tools/FtpColorsEditor.exe` that opens the same tab and mockup on their own, loading and saving gradient files. |
| `ColorPicker.py` (274) | `tools/ColorPicker.exe` (decided): the standalone RGBA picker (SV square, hue and alpha bars, spin boxes, hex entry, Fusion style), taking `r g b a` on the command line as now |

---

## 4. Design notes for the parts that can't be ported line by line

### 4.1 Global state

`AppState` is about 60 mutable class attributes read everywhere. Phase 1 makes it one `AppState` object
(`AppState& state()`) with the same fields, so ported code reads the same. As the HUD and engine are split, ride
state moves into `WorkoutEngine` and settings state into `SavedSettings`. Don't redesign it up front.

### 4.2 Themes without monkeypatching

The Python theme wraps `QWidget.setStyleSheet`, `QLabel.setText` and others so every stylesheet passes through the
current theme and its original is remembered. C++ can't patch Qt methods, so:

- `Theme::setSheet(QWidget*, const QString& css)` stores the untouched css in a dynamic property and sets the themed
  version. Every screen calls it instead of `setStyleSheet` (one reviewable call site per use; a test greps for raw
  `setStyleSheet` calls).
- `Theme::apply()` walks `QApplication::allWidgets()` and re-themes every widget that has the property, then lets the
  self-painting widgets (gauges, spiral, progress bars, plot labels) redraw from `Theme::color()`/`Theme::font()`.
- Colour maps, font roles (Orbitron and Segoe UI Black as markers), scale and weight, contrast/`text_on`, and the
  colours/gradients files stay identical, so every file in `themes/` and `defaultTheme.json` loads unchanged.

### 4.3 The ANT bridge thread

- `AntBridge` lives on a worker `QThread` and owns `antnative::Manager`. The loop: process pending connect/disconnect,
  service calibration, service trainer target, `poll()` messages (up to 64), handle each, then wait about 10 ms or until
  woken by a request (`QWaitCondition`).
- The Python bridge reacts to "profile changed" callbacks. In C++ it reacts to polled messages: each `ant_message`
  carries device id, page data, ticks and revolutions, and `sensors()` gives state, stalled, battery and capabilities.
  The reading pipeline does its own tick math from messages (as the poller does today), so the 513/2305 filter and
  distance counting stay the same.
- The watchdog runs inside AntNative (`start_recovery` with the same timeouts: stale 2.5, lost 5, reopen wait 5,
  reset 10, cooldown 12) and channel events already reach it in the core. Its log lines come from `next_log()` into
  the app log.
- For tests, `IAntManager` is a small interface with two implementations: `NativeAntManager` (antnative::Manager) and
  `FakeAntManager` (scripted sensors and messages, recorded sends, injected failures), playing the role
  `tests/fakes.py` does now.
- **Possible AntNative additions** (ANT_API_VERSION 3), only if the port shows a need: `ant_manager_wait(m, timeout_ms)`
  so the thread sleeps until a message arrives instead of polling every 10 ms, and a per-sensor `version`
  counter in `ant_sensor`.

### 4.4 Graphs: what they need, and the options

**What the two graphs use today** (`ClassWorkoutGraph.py`, the HUD's progress graph, the synopsis graph):

1. Two y scales over one time axis: power on the left; heart rate, cadence and speed on the right (two pyqtgraph
   ViewBoxes, x-linked)
2. Zone-coloured target columns, done and to-come shades (`SolidColoredColumns`, a custom item)
3. Sensor curves with a halo pen, dropouts drawn as zero, and on the synopsis a click (10 px tolerance) that highlights
   a curve
4. Vertical lines: dashed ride dividers, and the HUD's "now" line with caret ends
5. Diamond markers on each curve's latest reading
6. Overlays placed in data coordinates, some animated: `MaxStar` riding the curve to each new maximum, `MaxBubble`,
   max-value labels that keep clear of each other and of the data, ride and song strips (hover, click), grade regions
   (click, highlight), grade popup placement
7. Synopsis: x-only drag and wheel zoom held within limits (0 to the session length, a minimum zoom), range-changed
   signals that re-place the overlays, tick spacing that changes with zoom, time edits kept in sync
8. HUD: no mouse, a 720 s window that scrolls every 100 ms tick, the y range refitted to make room for the stars
9. Theming: every pen, axis text and font follows the theme live; transparent background inside a rounded frame

Points 2 and 6 are about 840 lines of the app's own code (`ClassMaxStar.py`, `ClassSolidColoredColumns.py`, the
synopsis's PlotLabel/GradeRegion/BandStrip, `ClassWorkoutGraph.py`), and they're all `QGraphicsItem`s already.
That's what separates the options: pyqtgraph supplies only the generic parts (view boxes, axes, curves, lines,
scatter, pan and zoom).

| Option | Custom items (2, 6) | What it gives | What we'd write | Licence / future | Fit |
|---|---|---|---|---|---|
| **A. In-house on QGraphicsView** | Port line for line: same `QGraphicsItem` paint code and the same data-to-scene mapping | Nothing; we own it | ViewBox (ranges, limits, pan, zoom, x-link), AxisItem (pyqtgraph's tick algorithm, time labels, grid), CurveItem (halo, hit test, per-pixel min/max thinning for long rides), InfiniteLine with carets, diamond scatter. About 1.5–2k lines. | No dependency | **Chosen.** Looks the same as today, because we control every pixel. |
| **B. Qwt 6.3** | Rewrite each as a `QwtPlotItem::draw(painter, xMap, yMap)`, with hover and click routed by hand | Left and right y axes, panner, magnifier, zoomer, `QwtScaleDraw` for time labels, curves, markers | The custom items again; build Qwt from source against `C:\Qt\6.7.0` (vcpkg would build its own Qt) | LGPL 2.1 + Qwt exception: subclassing and static linking are fine, and only changes to Qwt itself must be shared. Mature, slow-moving. | **Runner-up**, if you'd rather not own axis and zoom code. The canvas is QPainter, not a scene, so the animated stars repaint the plot each frame (cached canvas helps). |
| **C. Qt Charts** (installed) | Port mostly as-is: it's QGraphicsView too, mapped with `chart->mapToPosition()` | Axes, a secondary axis, line series, click signal, OpenGL option | Pan and zoom limits; time labels via a `QCategoryAxis` rebuilt on every range change; halos as a second series per curve; columns still custom | **Deprecated since Qt 6.10**, with no new features or minor fixes, and expected to be gone in Qt 7 | Not recommended: a dead end, and its plot margins, axes and chart theme won't match today's look. |
| **D. QCustomPlot 2.1** | Rewrite as `QCPAbstractItem`s | Multiple axes, per-axis drag and zoom, time tickers, layers with buffered replot (good for animation), fast; one .h/.cpp pair | The custom items again | As far as I know GPL v3, or a paid commercial licence (check before adopting). The repo has no licence file today; GPL would decide how the app may be shared if you ever distribute it. | Capable, but the licence attaches strings that A and B don't. |
| **E. Qt Graphs** (Charts' successor) | Rewrite in QML | Modern 2D graphs | Everything in Qt Quick inside a `QQuickWidget` | Needs Qt 6.8+, which breaks D2 (GDI fonts). Forum reports of missing axis-range features. | Not viable now. |
| **F. Plain QPainter widgets** | Rewrite as paint code with hand-written hit testing | Nothing | Everything, including hover and click routing on the synopsis | No dependency | Fine for the HUD graph (no mouse), poor for the synopsis. Possible as a hybrid with A. |

**Decided: A**, built only against what the two graphs use, not as a general library. Writing pyqtgraph's
generic parts is about the same work as adapting the 840 lines of custom items to Qwt's or QCustomPlot's item APIs,
and it leaves no dependency, an identical look, and items that port unchanged.

**De-risking it:** Phase 4 starts with a time-boxed spike that builds the HUD progress graph (no mouse, so only
points 1–5, 8, 9) on option A and checks it against the Python HUD in LayoutViewer. If the axes or curves fight back,
switch to B before the synopsis (points 6–7) is built. The custom items are written the same way under either.

### 4.5 Music analysis parity

Generate golden fixtures from Python once: for a few test tones (the hardware test's `write_tone` click tracks) and
two or three real songs, save the onset envelope, tempo track, beats, silences, segments and built workout as JSON.
The C++ tests compare within tolerance. If they match, the C++ app reuses `musicCadenceCacheANT.json` under the same
`cache_version`. If not, it uses its own cache version, so neither app trusts the other's results.

---

## 5. Phases

Each phase ends with a build, its tests passing, and a commit on a branch.

### Phase 0: scaffolding (done)
- Done: `build_windows.ps1` builds Release and Debug, and `dist\AntWorkout\` runs with no Qt on PATH and lists
  both ANT sticks. Cause of the Python DLL crash found: PyQt6's bundled C++ runtime (14.26) is older than the 14.40
  AntNative is built with. `dist\` ships the VS runtime, and `tst_ant_sticks` checks the version that gets loaded.
  windeployqt runs with `--no-network --no-system-dxc-compiler` (52 MB deploy).
- `AntWorkoutQt/CMakeLists.txt`: Qt 6.7.0 (Core, Gui, Widgets, Svg, Concurrent, Test), AntNative via
  `add_subdirectory` (Python module off), the library targets, an empty `AntWorkout.exe` window, and a Qt Test exe.
- `build_windows.ps1`: builds ANT_LIB if needed, configures and builds with Ninja, runs `windeployqt`, copies
  `antnative.dll` and the assets into `dist\`.
- Check that Qt and ANT_LIB DLLs load together in one process (the Python app needed an import-order hack for this).
- **Done when:** the empty window opens from `dist\`, a test runs headless, and `antnative::list_devices()` works
  from the exe.

### Phase 1: core logic (`awo_core`) + command-line tools (done)
- Done: `src/core` (the parser, ZWO conversion, FTP tests, grading, session, AppState, WorkoutData, settings, units,
  performance, calibration, logger and the FTP colour maths), `awo-verifier.exe` and `zwo2awo.exe`, and 9 test exes
  (784 cases). `tests/golden/make_golden.py` writes the Python app's results for every shipped and scratchpad workout
  at three FTPs, ZWO cases, grading scenarios, rings, units, and every theme's colours with every gradients file. The
  C++ results match them exactly, apart from the changes in `CHANGES.md`.
- Notes for later phases: `awo_core` links Qt Gui for `QColor` only, so colours round exactly as PyQt6's did. The
  theme-dependent half of FtpColors (the current palette, pens, block text contrast, watchers) comes with the Theme in
  Phase 3; the core functions take the ranges and palette explicitly, and `ZoneRanges.cadence` replaces the Python
  app's "is this the cadence zones" guesswork.
- Port §3.1. Extract the `.awo` parser out of the setup screen.
- **Golden tests against Python:** a small Python script (kept in `tests/golden/`) runs the Python modules over every
  file in `awoWorkouts/` and the test `.awo`/`.zwo` files, plus grading, FTP test and colour inputs, and writes JSON.
  The C++ tests must produce the same.
- Port the matching unit tests: workout files (24), helpers (15), grading (24), FTP test (11), session (7), settings
  (21), ZWO (23), logger (8), and the colour math half of colors-and-theme.
- `awo-verifier.exe` and `zwo2awo.exe` work like the scripts.
- **Done when:** golden output matches for every bundled workout, and the tools give the same results as the scripts.

### Phase 2: ANT layer (`awo_ant`) (done)
- Done: `AntBridge` (a QThread over the core's Manager and Recovery, with a transport factory) and `ReadingPipeline`
  (the poller's logic, emitting what to show). `tests/FakeAnt.hpp` is a fake `Transport` that feeds real raw ANT
  messages through the core's decoder. 39 bridge and 27 pipeline cases, ported from the Python tests.
  `hwtests/ant_console.exe` against SimulANT+: the sensors scenario (power 180 W, cadence 80, speed 17.4 mph, heart
  rate 110) and the trainer scenario (150 W target reached, heart rate 110) pass. It rescans 3 s after opening so
  devices the stick replays from its last session aren't picked.
- `IAntManager`, `NativeAntManager`, `FakeAntManager`, `AntBridge`, `ReadingPipeline`.
- Port the bridge tests (34) and poller tests (18) onto the fake.
- A small console harness (`hwtests/ant_console.exe`) that connects to SimulANT+ sensors and prints readings, for a
  first over-the-air check before any UI exists.
- **Done when:** fake-based tests pass, and against SimulANT+ the console shows power, cadence, speed and heart rate,
  a trainer follows target power, and dropouts recover.

### Phase 3: UI foundation (done)
- Done: `src/ui`: Theme (setSheet/apply over the same theme files and regexes), ZoneColors (FtpColors' theme half),
  AppLayout (scaling, gap grid, text fitting, symbol buttons, popup shrinking; screens registered by name),
  KeyShortcuts, SoundEffects (miniaudio, vendored in `third_party/`), CustomTitleBar, the small dialogs and a
  MainWindow with no screens yet. 3 more test exes (theme, key shortcuts, layout and windows). Pick Theme opens a file
  picker until the theme picker is built (Phase 9). Stylesheets select the app's own widgets by object name, as Qt
  type selectors don't work with namespaced classes.
- Theme (§4.2), AppStyle, font registration (the `fonts/` and `fonts/variable/` folders), AppLayout, KeyShortcuts,
  SoundEffects, CustomTitleBar, MainWindow screen switching, and the small dialogs (confirm exit/end/clear session,
  logs, measurements, performance).
- Port the key shortcut tests (16) and the theme half of colors-and-theme.
- **Done when:** the window comes up maximized and frameless with the title bar menu, themes load and switch live,
  and the layout scales on 1080p, 1200p and 4K (and with 125%/150% Windows scaling).

### Phase 4: custom widgets + plot library (done)
- Done: `src/plot` (awo_plot: ViewBox, AxisItem, PlotItem/PlotWidget, curves, the now line, scatter markers and
  SolidColoredColumns) and `src/ui/widgets` (AnalogGauge and its Gauge backgrounds, PowerSpiral, GradeOverlay/GradeList,
  SmoothProgressBar, LongVLabels, WorkoutGraph, MaxStar/MaxBubble). The graph spike kept option A: the HUD graph
  matches the Python one to 10 pixels of 360,000 (lone pixels on the aliased curves), and every other widget is pixel
  identical. `tests/parity` renders each widget from the Python app and from `render_widgets.exe` with the same setup
  and readings, and compares them (see README); it stands in for LayoutViewer's widget gallery until Phase 9. 2 more
  test exes: tst_plot (ticks and view ranges against pyqtgraph's own, `golden/plot.json`) and tst_widgets (the power
  spiral's tests, the gauge's easing rate and the other widgets).
- Changed from the plan: QSpiralProgressBar (809 lines) isn't ported: nothing in the app builds one (GradeOverlay
  only ever covers the PowerSpiral). MaxStar/MaxBubble live in `ui/widgets`, not `plot`, as they draw in the theme's
  colors. The power spiral test of the HUD's sampling moves to Phase 6 with the HUD.
- First, the graph spike (§4.4): the HUD progress graph on the in-house library, compared with the Python one. Keep
  option A or switch to Qwt here.
- AnalogGauge, SpiralProgressBar, PowerSpiral, SmoothProgressBar, LongVLabels, GradeOverlay/GradeList, `awo_plot`,
  WorkoutGraph, SolidColoredColumns, MaxStar/MaxBubble.
- A widget gallery page in LayoutViewer (start of Phase 9's viewer) to see each widget in isolation.
- Port the power spiral tests (19).
- **Done when:** each widget renders side by side with the Python version at the same size and values with no
  visible difference (see §6), and the gauge animates at the configured FPS without dropped frames.

### Phase 5: setup screen and its dialogs (done)
- Done: `ui/screens/SetupScreen` and `ui/dialogs` (SelectDevicesWindow, ConnectDevicesWindow, WorkoutSelectDialog,
  CalibratePowerWindow), the setup screen built by MainWindow. Workout files and FTP tests are read with
  `core/AwoParser`. The whole screen and each popup render pixel identical to the Python app's from the same state
  (tests/parity). tst_setup (24 cases, on a stand-in bridge) covers auto-select and auto-connect, the settle wait,
  the status and blinking, the settings and their checks, the modes and FTP tests, workout files loaded and rejected,
  picking devices by hand, the connect prompt, the workout picker and calibration. On SimulANT+ the app auto-picked
  and connected the power meter, heart rate monitor and speed/cadence sensor ("Connected Succesfully"), and left the
  power slot to the rider while a second power source (the FE-C trainer) was on the air.
- Waiting on later phases: START comes straight back to the setup screen until the workout screen exists (Phase 6),
  and Musical Cadence Mode's Select Music does nothing until the music work (Phase 8).
- SetupScreen, SelectDevices, ConnectDevices, WorkoutSelect, CalibratePower.
- **Done when:** with the fake and with SimulANT+, scanning, auto-select after the settle time, manual selection,
  connect, disconnect and rescan, FTP/HR/wheel/weight entry, all modes (workout, free, FTP tests), workout loading
  and errors, and power calibration behave as in Python.

### Phase 6: workout engine + HUD (done)
- Done: `core/WorkoutEngine` (the workout logic out of ClassHUDWindow.py) and `ui/screens/HudScreen` on it, with
  the END prompt (`ui/dialogs/ConfirmEndWorkoutWindow`) and MainWindow's setup -> workout screen -> back to setup.
  golden/hud.json's rides (a mixed workout with difficulty changes and a pause, the 20m test, a ramp test, ridden by
  the same simulated rider and seed) match every power request and grade field, both on the engine
  (tst_workout_engine) and through the whole screen (tst_hud, 24 cases: also the spiral's samples, START/PAUSE/END,
  workouts ridden to the end, the trainer following the workout). The screen 600 s into a workout renders the same
  as the Python one (tests/parity `hud_screen`: 31 pixels differ, all along the graph's 1 px curves); the plot now
  lays itself out at the widget's size from the start, as pyqtgraph does, so a range set before it's shown is
  padded the same.
- The SimulANT+ trainer scenario passes on the C++ app: `hwtests/simulant_ride.exe` rides it (the real bridge,
  readings pipeline and workout screen) and `hwtests/simulant_test.py` reuses the Python harness's timeline, scripts
  and checks. 22/22 twice running at 85% reception, the same as the Python app's run beside them; three earlier
  runs failed one timing check each (the first target late, or the trainer slow to come back), which the Python
  app's runs show too now and then (3 of its 9 trainer runs).
- Waiting on later phases: Exit to Synopsis adds the ride to the session and goes back to setup until the synopsis
  exists (Phase 7); Musical Cadence Mode's music (Phase 8).
- `core/WorkoutEngine` first, with tests: block timing, ramps, intervals, messages, ERG target and the power drive
  cutoff, difficulty, pause/resume/restart, completion, FTP test result, grading hooks.
- Then `HudScreen` on top of it: gauges, graph, spiral, block labels, up next, remaining list, sliders, battery
  warnings, start/pause/end buttons, the end-workout prompt.
- **Done when:** a full ride with the fake rider (the PreviewRide `Rider`) matches the Python HUD's recorded power
  requests and grades for the same workout and seed, and the SimulANT+ trainer scenario passes.

### Phase 7: synopsis (done)
- Done: `ui/screens/SynopsisScreen` (and `ui/widgets/SynopsisItems`: the max labels, graded blocks and strips), built
  by MainWindow on Exit to Synopsis, with Return to Setup and Exit. The synopsis of a whole simulated workout renders
  the same as the Python one (tests/parity `synopsis_screen`: 134 pixels differ, along the 1 px curves). The plot
  library gained what the synopsis' three overlapping views need from pyqtgraph: links that go both ways (the view
  on top takes a drag, the others follow) and clicks handed down through the views to the curve, label, strip or
  graded block under them (`plot::Clickable`). tst_synopsis (13 cases) covers the stats and grade, the max stars
  riding to a selection's max, zooming and the time boxes, dragging, clicks, grade popups and Left/Right, Block
  Grades, two rides' strip, an FTP test's result and Apply FTP, Set Max HR and best power over dropouts;
  tst_app_flow ports the 5 app flow tests (the whole app on the fake stick: auto-connect, rides, grades, the
  synopsis, back to setup, the exit, the trainer, a two-ride session and Clear Session).
- Waiting on Phase 8: Musical Cadence Mode's song strip and the songs in the grades are built but only tested once
  there are songs (test_cadence_synopsis_shows_the_songs_heard). Where two spots for a max label are exactly as near,
  the C++ app tries them left to right, top to bottom; Python tried them in its set's order (no visible difference
  seen).
- SynopsisScreen with session rides, strips, grade regions and popups, block grades sheet, zoom and time edits, max
  markers and animated stars, FTP test result and Apply FTP, set max HR.
- Port the screens tests (8) and app flow tests (5): setup, HUD, synopsis, back to setup, exit.
- **Done when:** the whole app flow works end to end on the fake, and multi-ride sessions show as in Python.

### Phase 8: Musical Cadence Mode (done)
- Done: `music/` (awo_music: Playlist, TempoAnalysis with the decoder, AnalysisCache, CadenceWorkout, MusicPlayer),
  `ui/dialogs/MusicSelectDialog`, the setup screen's music workout (from the picker, or last time's songs straight
  from the cache), and the workout screen's music (the player as the clock, the song in the title bar, the beat on
  the cadence gauge's rings, the volume, the songs heard for the synopsis' strip). Vendored: pocketfft as numpy 1.26.4
  builds it, and miniaudio updated to 0.11.25 (the Python app's pyminiaudio's).
- The analysis isn't just close to the Python app's, it's the same: `music/NumpyLike` does numpy's sums pairwise as
  numpy does, np.convolve through numpy's small_correlate and OpenBLAS's Skylake-X ddot (fused multiply-adds), and the
  FFTs are numpy's own pocketfft. golden/music.json (click tracks made from Python's random, so the C++ test rebuilds
  the same wav files byte for byte) matches to the bit: every onset envelope value, the tempo tracks, beats, silences,
  segments, rides and random picks. Real MP3s analyzed the same too (classical, surf rock, a 47 minute album), so the
  two apps share musicCadenceCacheANT.json under the same cache_version. (The match depends on numpy using OpenBLAS's
  AVX-512 kernel; on another CPU the Python app's own numbers could differ in the last bit.)
- golden/hud.json gains a Musical Cadence Mode ride; the workout screen rides it as the Python one does (requests,
  grades, the songs heard). Tests: tst_music (57, the 32 Python ones among them), the song picker and the setup
  screen's music workout in tst_setup, the synopsis' song strip in tst_synopsis. The song picker and the cadence
  workout screen render as the Python ones (tests/parity `music_picker`: identical; `cadence_screen`: 11 pixels along
  the curves). The player plays on the real sound device and its clock keeps time (tst_music with AWO_SOUND set).
- Not done here: an audible ride on real songs in the app (it plays out loud; for the rider to try).
- Decoder, TempoAnalysis, cache, playlist and selection files, CadenceWorkout, MusicPlayer; MusicSelectDialog; HUD
  music integration (song title, beat pulses, volume, the music position as the clock); synopsis song strip.
- Port the music cadence tests (32), plus the golden comparisons in §4.5.
- **Done when:** golden fixtures match within tolerance, and a cadence ride with real songs plays and stays in step.

### Phase 9: themes picker, LayoutViewer, tools
- ThemeSelectDialog with the live preview (PreviewRide, fake bridge, ride sandbox).
- LayoutViewer: every screen and mode without a stick, font/colour/theme editing, save as default, scenarios,
  screenshots.
- LayoutViewer's FTP Colors tab and mockup window (`FtpColorsEditor.py`), plus `FtpColorsEditor.exe` for running
  them on their own, and `ColorPicker.exe` (§3.7).
- Port the theme picker tests (4).
- **Done when:** themes made in the C++ viewer load in the Python app and the other way round.

### Phase 10: hardware tests, packaging, switch-over
- Port `run_simulant_test.py` (done in Phase 6: `hwtests/simulant_test.py` + `simulant_ride.exe`, also to run for
  the sensors scenario here) and `run_app_test.py` to C++ harnesses in `hwtests/` that link the app libraries and
  drive the real screens, as the Python ones import the real modules. The SimulANT+ scripts and the PowerShell UI
  automation (`simulant_automation/`) are reused unchanged.
- `dist\AntWorkout\` with `AntWorkout.exe`, `LayoutViewer.exe`, Qt DLLs, `antnative.dll`, fonts, sounds, svgs,
  themes and workouts. Optional: an installer.
- Run the parity checklist (§6). Then update the READMEs to make the C++ app the main one. `AntWorkout/` stays in the
  repo as the reference (D3).

---

## 6. How parity is checked

1. **Golden data:** Python writes JSON for parsing, conversion, grading, FTP tests, colours, music analysis and
   recorded rides. The C++ tests compare against it.
2. **Screenshots:** the Python LayoutViewer can save screenshots (`layoutScreenshots/`). Render the same screen,
   mode, theme and progress in both viewers at the same size and diff the images with a small tolerance (font
   antialiasing). Run it for every screen, a few themes and three screen sizes. Any difference is either a bug or an
   improvement listed in `CHANGES.md` (D10).
3. **Ported unit tests:** about 330 of the 400 Python tests carry over. The 73 that test the Python `ant` package
   don't (see §3.2).
4. **Hardware:** both SimulANT+ scenarios (sensors, trainer) pass with the C++ app.
5. **Files:** settings, the music cache and selection, themes, the log and the workout lists are read and written by
   both apps without loss.

---

## 7. Risks

| Risk | Mitigation |
|---|---|
| The pyqtgraph replacement grows | Build only what the synopsis and HUD graph use (§4.4). Fall back to Qt Charts for one plot if a piece gets stuck. |
| The theme engine misses widgets that set their own sheets | One `Theme::setSheet` call site, a test that fails on raw `setStyleSheet`, and the screenshot diff with a strong theme |
| Text fitting differs (font metrics) | Stay on Qt 6.7.0 (GDI). The screenshot diff catches drift early (Phase 3). |
| Music analysis doesn't match numpy | pocketfft, golden fixtures, and a separate cache version as the fallback |
| HUD timing and threading differ | The engine runs off its own monotonic clock (`QElapsedTimer`) and is unit tested. ANT data crosses threads only by queued signals. |
| Scope (the HUD, setup and synopsis are 6k lines between them) | Phases 1–2 give working, tested logic before any big screen. The engine split shrinks the HUD. |
| Qt and ANT_LIB DLL conflict | Checked in Phase 0 before anything else is built |

---

## 8. Decisions log

Decided:
- LayoutViewer, the FTP colours editor (also as its own exe) and ColorPicker are all ported.
- Improvements are allowed as long as functionality is preserved (D10).
- The Python app stays in the repo.
- Graphs use the in-house library (§4.4, option A), confirmed by the Phase 4 spike.

Nothing is open.
