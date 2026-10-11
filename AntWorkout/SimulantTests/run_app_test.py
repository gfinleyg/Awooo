"""
AntWorkout full app test against SimulANT+ (see README.md in this folder).

Where run_simulant_test.py checks the readings on the workout screen in Free Mode, this drives the whole app the way
a rider would - the real MainWindow, setup screen, workout screen and synopsis - against the same SimulANT+ scripts,
which send their steady "pairing" values the whole time (no timeline):

  python SimulantTests\\run_app_test.py trainer   FE-C trainer + heart rate: Workout Mode on the trainer (ERG
                                                  following the blocks, a ramp, intervals, a block over the Power
                                                  Drive Cutoff, a difficulty change, a pause), the synopsis, back to
                                                  setup, exit
  python SimulantTests\\run_app_test.py sensors   power meter + speed/cadence + heart rate: Musical Cadence Mode with
                                                  music analyzed and played (silently), a shortened 20m FTP test and
                                                  Apply FTP, Free Mode, exit

The simulators to have running are the same as run_simulant_test.py's for that scenario (and only those: a trainer
heard alongside a power meter makes the power device a choice to make on the setup screen).
Options: --offscreen (no windows), --wait MINUTES (how long to wait for the devices, default 10)
"""

import argparse
import datetime
import json
import os
import sys
import time
import traceback
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, "simulant_scripts", "modules"))
import awo_timeline as tl  # noqa: E402

NEEDS = {"trainer": {17, 120}, "sensors": {11, 121, 120}}
#steady values the scripts send without a timeline (awo_timeline.PAIRING)
PAIRING_POWER = tl.PAIRING["pm"]["power"]
FTP = 250

#the trainer test's workout: (seconds from the start of the workout it's in, the watts the trainer should be asked for)
#are checked against what the trainer script logged it received
TRAINER_WORKOUT = """T:App test on the trainer
1:30,0.6
2:30,0.6,1.0
3:2;1:15,1.1;1:10,0.5
1:20,1.4
1:20,0.8|Last block|10|0
"""


def say(text):
    print(time.strftime("%H:%M:%S ") + text, flush=True)


def write_tone(path, seconds, bpm):
    """A song for Musical Cadence Mode: a click on every beat at bpm, over a quiet hum"""
    import numpy as np
    rate = 22050
    t = np.arange(int(rate*seconds))/rate
    samples = 0.02*np.sin(2*np.pi*110*t)
    beat = 60/bpm
    for start in np.arange(0, seconds, beat):
        i = int(start*rate)
        n = min(int(0.03*rate), len(samples) - i)
        samples[i:i+n] += 0.8*np.sin(2*np.pi*1500*np.arange(n)/rate)*np.exp(-np.arange(n)/(0.008*rate))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((np.clip(samples, -1, 1)*30000).astype(np.int16).tobytes())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", choices=sorted(NEEDS))
    parser.add_argument("--offscreen", action="store_true")
    parser.add_argument("--wait", type=float, default=10.0)
    args = parser.parse_args()
    if args.offscreen:
        os.environ["QT_QPA_PLATFORM"] = "offscreen"

    runDir = os.path.join(tl.RUN_DIR, datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S_app_") + args.scenario)
    os.makedirs(runDir)
    #no epoch: the scripts keep sending their pairing values, and log to this run's folder
    control = {"run": runDir, "scenario": args.scenario, "epoch": None}
    with open(tl.CONTROL_FILE + ".tmp", "w") as f:
        f.write(tl.control_text(control))
    os.replace(tl.CONTROL_FILE + ".tmp", tl.CONTROL_FILE)
    #the app's own files (settings, log, caches) in the run folder, starting from known settings; the music silent
    os.chdir(runDir)
    with open("savedSettingsANT.json", "w") as f:
        json.dump({"wheelCircumference": tl.WHEEL_MM/1000, "ftp": FTP, "maxHr": 185, "maxWatts": 1200,
                   "powerDriveCutoff": 300, "musicVolume": 0}, f)
    say(f"Run folder: {runDir}")

    #the same start up as AntWorkout.py
    sys.path.insert(0, os.path.join(PROJECT, "AntNative", "python"))
    sys.path.insert(0, PROJECT)
    import ant.manager  # noqa: F401  (before PyQt6)
    import ClassAntLogger
    ClassAntLogger.install_error_logging()
    errors = []
    previousHook = sys.excepthook
    def hook(t, v, tb):
        errors.append("".join(traceback.format_exception(t, v, tb)))
        previousHook(t, v, tb)
    sys.excepthook = hook

    from PyQt6.QtCore import QTimer
    from PyQt6.QtGui import QFont
    from PyQt6.QtWidgets import QApplication
    app = QApplication(sys.argv)
    import AppStyle
    import AppTheme
    import SoundEffects
    import FtpTest
    import MusicCadence
    import SavedSettings
    from AppState import AppState
    from ClassMainWindow import MainWindow
    AppTheme.install()
    SoundEffects.init(app)
    AppState.defaultFont = QFont(app.font())
    app.setStyleSheet(AppStyle.APP_STYLESHEET)
    AppTheme.clear()
    window = MainWindow()
    window.show()
    bridge = window.startExe
    setup = window.setupWidget

    results = []
    def check(cond, text):
        results.append((bool(cond), text))
        say(("PASS  " if cond else "FAIL  ") + text)

    #------------------------------------------------------------------------------------------------
    #the test as a list of steps, each run every 100ms until it returns True (done) or its deadline passes
    #------------------------------------------------------------------------------------------------
    steps = []
    def step(name, seconds=60):
        def add(fn):
            steps.append((name, seconds, fn))
            return fn
        return add

    ctx = {}

    def hud():
        return getattr(window, "workoutHudWidget", None)

    def synopsis():
        return getattr(window, "synopsisWidget", None)

    @step("ANT stick open and scanning", 60)
    def _():
        return bridge.is_initialized()

    @step("SimulANT+ devices heard", args.wait*60)
    def _():
        found = {int(entry.split()[0]) for entry in bridge.found_devices()}
        if "announced" not in ctx:
            ctx["announced"] = True
            say(f"Waiting for {sorted(NEEDS[args.scenario])} (only these running in SimulANT+, sending pairing values)")
        extra = found & ({11, 17} - NEEDS[args.scenario])
        if extra:
            raise RuntimeError(f"Also hearing {sorted(extra)}: turn that simulator off for this scenario")
        return NEEDS[args.scenario] <= found and bridge.scan_settle_remaining() == 0

    if args.scenario == "trainer":
        @step("setup screen auto-selects the trainer and heart rate monitor", 20)
        def _():
            setup.init_ant_scan_connect()
            return setup.pwrSelected and setup.hrmSelected

        @step("Connect Selected Devices (no speed/cadence sensor, so not automatic)", 5)
        def _():
            setup.connect_selected_devices()
            setup.w.connectForSureButton.click()
            return True
    @step("connected", 30)
    def _():
        return bridge.is_ready()

    @step("setup screen ready", 10)
    def _():
        setup.init_ant_scan_connect()
        ctx["roles"] = sorted(bridge.selected_roles())
        return True

    def start_ride():
        """The setup screen's START, then the workout screen's START (workout modes)"""
        check(setup.introStartButton.isEnabled(), "setup START enabled ("
              + ((AppState.workout_display_name() or "workout") if AppState.workoutMode else "Free Mode") + ")")
        setup.introStartButton.click()
        if AppState.workoutMode:
            hud().startEndButton.click()
        ctx["rideStart"] = time.time()

    def end_ride(escape=False):
        """END (or Esc, which must bring up the same End Workout prompt), then Exit to Synopsis"""
        from ClassConfirmEndWorkoutWindow import ConfirmEndWorkoutWindow
        if escape:
            window.escape_pressed()
            prompt = getattr(hud(), "w", None)
            check(isinstance(prompt, ConfirmEndWorkoutWindow) and prompt.isVisible(), "Esc on the workout screen: "
                  "End Workout prompt")
        else:
            hud().startEndButton.click()
        hud().w.endWorkoutConfirmButton.click()

    def back_to_setup():
        """the synopsis' RESTART: back to the setup screen, which disconnects and scans again"""
        synopsis().SynopsisRestartButton.click()
        ctx["backAt"] = time.time()

    def screens():
        """the main window's screens showing (only ever one)"""
        return [name for name, widget in (("setup", setup), ("workout", hud()), ("synopsis", synopsis()))
                if widget is not None and widget.isVisible()]

    def only_setup_showing():
        #(checked once the synopsis' own timer a second after it's built has been: it used to show itself again)
        check(screens() == ["setup"], f"back on setup: only the setup screen showing ({screens()})")

    def reconnected():
        """after going back to setup: connected again (all by itself with a speed/cadence sensor; the trainer test
        presses Connect Selected Devices as before)"""
        if not setup.isVisible():
            return False
        if args.scenario == "trainer" and not bridge.is_connect_requested() and bridge.scan_settle_remaining() == 0:
            setup.connect_selected_devices()
            setup.w.connectForSureButton.click()
        if bridge.is_ready() and time.time() - ctx["backAt"] > 1.5:
            setup.init_ant_scan_connect()
            only_setup_showing()
            return True
        return False

    #------------------------------------------------------------------------------------------------
    if args.scenario == "trainer":
        @step("Workout Mode: load the test workout", 10)
        def _():
            path = os.path.join(runDir, "app_test.awo").replace("\\", "/")
            with open(path, "w") as f:
                f.write(TRAINER_WORKOUT)
            #(as the workout picker's Ride This Workout does)
            ok = setup.process_workout_file(path)
            AppState.workoutFilename, AppState.workoutFilenameShort = path, os.path.basename(path)
            setup.workoutFileSelected.setText(" " + AppState.workout_display_name() + " ")
            setup.workoutDisplayLabel.show()
            setup.check_enable_start_button()
            check(ok and AppState.workoutDurationSeconds == 150, f"workout loaded, {AppState.workoutDurationSeconds}s")
            return True

        @step("ride starts", 10)
        def _():
            start_ride()
            ctx["hudStart"] = time.time()
            return True

        @step("riding (difficulty +2 at 25s, a pause from 42s to 50s)", 200)
        def _():
            h = hud()
            t = h.workoutSecondsPassed
            if t >= 25 and "difficulty" not in ctx:
                ctx["difficulty"] = time.time()
                h.difficultySlider.setValue(2)
                say("difficulty +2 (FTP 260)")
            if t >= 42 and "paused" not in ctx:
                ctx["paused"] = time.time()
                h.pauseButton.click()
                say("paused")
            if "paused" in ctx and "resumed" not in ctx and time.time() - ctx["paused"] >= 8:
                ctx["resumed"] = time.time()
                h.pauseButton.click()
                say("resumed")
            if h.workoutCompleteShown:
                ctx["completed"] = time.time()
                return True
            return False

        @step("cool down a few seconds, then END to the synopsis", 20)
        def _():
            if time.time() - ctx["completed"] < 4:
                return False
            ctx["grades"] = dict(AppState.blockGrades)
            end_ride(escape=True)
            ctx["ended"] = time.time()
            return True

        @step("synopsis", 10)
        def _():
            s = synopsis()
            if s is None or not s.isVisible():
                return False
            from WorkoutGrading import workout_average, letter
            grades = ctx["grades"]
            average = workout_average(grades)
            pcts = sorted(g["pct"] for g in grades.values() if g["pct"] is not None)
            #every block and interval rep graded; the one over the Power Drive Cutoff (trainer released, riding its
            #free 120W against 364W asked) grades low, the rest were ridden at the target by the trainer
            check(len(pcts) == 8, f"workout graded: {len(pcts)} of 8 blocks/reps, average {average and round(average, 1)}%"
                                  f" ({letter(average)})")
            check(pcts and pcts[0] < 60 and all(p >= 90 for p in pcts[1:]),
                  f"grades: released block {pcts and round(pcts[0])}%, the rest {[round(p) for p in pcts[1:]]}")
            check(s.bestPowerRange and not s.dropoutsKeepAverage, f"synopsis best power {s.bestPowerRange[:3]}")
            ctx["synopsisAt"] = time.time()
            return True

        @step("trainer commands", 15)
        def _():
            if time.time() - ctx["synopsisAt"] < 4: #(the stop's 0 W gets a moment to arrive)
                return False
            check_trainer_commands()
            return True

    def check_trainer_commands():
        start = ctx["hudStart"]
        log = []
        with open(os.path.join(runDir, "simulant_fe.csv")) as f:
            for line in f:
                t, event, detail = line.rstrip("\n").split(",", 2)
                if event in ("rx_target", "rx_resistance"):
                    log.append((float(t) - start, event, float(detail)))
        targets = [(t, w) for t, e, w in log if e == "rx_target"]
        def got(watts, after, before=10**9, tolerance=0.6):
            return next((t for t, w in targets if after <= t <= before and abs(w - watts) <= tolerance), None)
        check(got(150, 0, 10) is not None, "first block: trainer got 150W")
        ramp = sorted({round(w) for t, w in targets if 150 < w < 250})
        check(len(ramp) >= 10, f"ramp: trainer followed it up ({len(ramp)} steps between 150 and 250W)")
        diff = ctx["difficulty"] - start
        check(any(abs(w - round(w)) < 0.01 and t > diff for t, w in targets),
              "targets keep coming after the difficulty change")
        pause = ctx["paused"] - start
        check(got(0, pause - 1, pause + 4) is not None, "pause: trainer target 0W")
        #intervals at FTP 260: 286W / 130W (under the 300W Power Drive Cutoff)
        check(got(286, 60) is not None and got(130, 60) is not None, "intervals: trainer got 286W and 130W (difficulty +2)")
        released = [t for t, e, w in log if e == "rx_resistance" and w == 0]
        check(released, "a block over the Power Drive Cutoff (364W > 300W) released the trainer"
                        + (f" at {released[0]:.0f}s" if released else " - never"))
        check(got(364, 0) is None, "the trainer was never asked for the 364W over the cutoff")
        check(got(208, 100) is not None, "last block: trainer got 208W")
        end = ctx["ended"] - start
        check(got(0, end - 1, end + 5) is not None, "end of the ride: trainer target 0W")

    #------------------------------------------------------------------------------------------------
    if args.scenario == "sensors":
        @step("Musical Cadence Mode: songs analyzed (worker processes)", 120)
        def _():
            songs = []
            for name, bpm in (("App Test - Ninety", 90), ("App Test - One Twenty", 120)):
                path = os.path.join(runDir, name + ".wav")
                write_tone(path, 25, bpm)
                songs.append((path, name))
            analyses = MusicCadence.analyze_songs([p for p, _ in songs])
            bad = [p for p, a in analyses.items() if "error" in a]
            check(not bad and len(analyses) == 2, f"songs analyzed ({len(analyses)}, unreadable {len(bad)})")
            setup.musicSelection = {"songs": songs, "analyses": analyses, "preferred": 85, "minSegment": 8}
            setup.cadenceModeButton.click()
            check(AppState.cadenceMode and AppState.workoutList and len(AppState.musicTracks) == 2,
                  f"music workout built: {AppState.workoutDurationSeconds}s, "
                  f"cadences {sorted({b[2][0] for b in AppState.workoutList if b[0] == 1})}")
            return True

        @step("music ride starts", 10)
        def _():
            start_ride()
            return True

        @step("riding to the music", 120)
        def _():
            h = hud()
            if "musicAt" not in ctx and h.musicPlayer is not None and h.workoutSecondsPassed > 5:
                ctx["musicAt"] = (time.time(), h.workoutSecondsPassed)
            if "App Test - " in window.title_bar.title.text():
                ctx["titleSeen"] = window.title_bar.title.text().strip()
            if h.workoutCompleteShown:
                return True
            return False

        @step("music ride ends", 20)
        def _():
            h = hud()
            elapsed = time.time() - ctx["musicAt"][0]
            moved = AppState.workoutDurationSeconds - ctx["musicAt"][1]
            check(abs(elapsed - moved) < 2.5, f"the workout clock followed the music ({moved:.1f}s of ride in "
                                               f"{elapsed:.1f}s)")
            check(h.musicPlayer is None or not h.musicPlayer.skipped, "no song skipped")
            check(ctx.get("titleSeen"), f"title bar showed the song playing ({ctx.get('titleSeen')})")
            ctx["cadenceGrades"] = dict(AppState.blockGrades)
            end_ride(escape=True)
            return True

        @step("music synopsis", 10)
        def _():
            s = synopsis()
            if s is None or not s.isVisible():
                return False
            check(s.cadence_ride() and ctx["cadenceGrades"], f"cadence ride synopsis, {len(ctx['cadenceGrades'])} "
                                                             "blocks graded")
            back_to_setup()
            return True

        @step("back on the setup screen: connected again", 30)
        def _():
            return reconnected()

        @step("20m FTP test (shortened: 20s warmup, 40s effort)", 15)
        def _():
            check(setup.blinkTimer.isActive(), "setup screen blinking again after the ride")
            FtpTest.WARMUP_LINES = ["1:20,0.5"]
            FtpTest.WARMUP_SECONDS = 20
            FtpTest.TESTS["20m"]["effortSeconds"] = 40
            setup.ftp20mButton.click()
            check(AppState.ftpTestMode == "20m" and AppState.workoutDurationSeconds == 60,
                  f"FTP test built, {AppState.workoutDurationSeconds}s")
            start_ride()
            return True

        @step("FTP test effort", 90)
        def _():
            return hud().workoutCompleteShown

        @step("FTP test result", 15)
        def _():
            result = AppState.ftpTestResult or {}
            average = result.get("average")
            check(result.get("completed") and average is not None and abs(average - PAIRING_POWER) <= 5,
                  f"FTP test average {average and round(average, 1)}W (the power meter sends {PAIRING_POWER}W)")
            end_ride()
            return True

        @step("FTP test synopsis and Apply FTP", 10)
        def _():
            s = synopsis()
            if s is None or not s.isVisible():
                return False
            expected = FtpTest.ftp_from_average("20m", AppState.ftpTestResult["average"])
            check(s.dropoutsKeepAverage, "20m test: best power keeps its average through dropouts")
            check(s.newFtp == expected and s.applyFtpButton.isEnabled(), f"new FTP {s.newFtp} offered")
            s.applyFtpButton.click()
            check(AppState.ftp == str(expected) and SavedSettings.load().get("ftp") == expected,
                  f"Apply FTP: FTP is {AppState.ftp} and saved")
            back_to_setup()
            return True

        @step("back on the setup screen: connected again ", 30)
        def _():
            return reconnected()

        @step("Free Mode ride (15s)", 40)
        def _():
            if "freeStart" not in ctx:
                setup.freeModeButton.click()
                start_ride()
                ctx["freeStart"] = time.time()
                return False
            if time.time() - ctx["freeStart"] < 15:
                return False
            h = hud()
            check(h.wattGaugeDisplay.ui.widget.reading == PAIRING_POWER,
                  f"free ride power {h.wattGaugeDisplay.ui.widget.reading}W")
            check(abs(float(h.currentSpeed) - tl.PAIRING["sc"]["speed"]/1.609344) < 0.6,
                  f"free ride speed {h.currentSpeed} mph")
            end_ride()
            return True

        @step("free ride synopsis", 10)
        def _():
            s = synopsis()
            return s is not None and s.isVisible()

    #------------------------------------------------------------------------------------------------
    @step("back to setup", 15)
    def _():
        if "back" not in ctx:
            ctx["back"] = True
            back_to_setup()
            return False
        if not setup.isVisible() or time.time() - ctx["backAt"] < 1.5:
            return False
        only_setup_showing()
        check(setup.blinkTimer.isActive() and setup.timer.isActive(), "setup screen timers running again")
        return True

    @step("exit (the app's own: ANT stick closed, then out of the event loop)", 15)
    def _():
        #(Esc on the setup screen is the X: the exit prompt)
        if args.scenario == "trainer":
            window.escape_pressed()
        else:
            window.close()
        prompt = window.w
        check(prompt.isVisible(), "exit prompt shown")
        #Yes, Exit (it quits the event loop, which ends app.exec() below as it does in AntWorkout.py)
        prompt.exitSynopsisConfirmButton.click()
        check(not bridge.isRunning(), "ANT bridge stopped")
        check(not any(w.isVisible() for w in QApplication.topLevelWidgets()), "every window hidden")
        #(the report now: the event loop ends before the next step would run)
        state["index"] = len(steps)
        timer.stop()
        finish(0)
        return True

    #------------------------------------------------------------------------------------------------
    state = {"index": 0, "deadline": None}

    def finish(code):
        if state.get("finished"):
            return
        state["finished"] = True
        log = ClassAntLogger.read_log(5000)
        unhandled = log.count("Unhandled exception")
        check(not errors and not unhandled, f"no Python errors ({len(errors)} caught, {unhandled} in the app's log)")
        passed = sum(1 for ok, _ in results if ok)
        lines = [f"AntWorkout full app test: {args.scenario}   {datetime.datetime.now():%Y-%m-%d %H:%M}",
                 f"Devices: {ctx.get('roles')}", ""]
        lines += [("PASS  " if ok else "FAIL  ") + text for ok, text in results]
        lines += ["", f"{passed}/{len(results)} checks passed"]
        lines += errors[:5]
        report = "\n".join(lines)
        with open(os.path.join(runDir, "report.txt"), "w", encoding="utf-8") as f:
            f.write(report + "\n")
        print("\n" + report, flush=True)
        app.exit(code if code else (0 if passed == len(results) else 1))

    def tick():
        if state["index"] >= len(steps):
            timer.stop()
            finish(0)
            return
        name, seconds, fn = steps[state["index"]]
        if state["deadline"] is None:
            state["deadline"] = time.time() + seconds
            say(f"- {name}")
        try:
            done = fn()
        except Exception:
            errors.append(traceback.format_exc())
            say(f"Test error in '{name}':\n" + traceback.format_exc())
            timer.stop()
            finish(3)
            return
        if done:
            state["index"] += 1
            state["deadline"] = None
        elif time.time() > state["deadline"]:
            check(False, f"{name}: timed out")
            timer.stop()
            finish(2)

    timer = QTimer()
    timer.timeout.connect(tick)
    timer.start(100)
    code = app.exec()
    if not state.get("finished"):
        #(the event loop ended before the test did)
        check(False, f"app quit early, in step '{steps[state['index']][0]}'")
        finish(1)
        code = 1
    if bridge.isRunning():
        bridge.stop()
        bridge.wait(5000)
    #(out without Python's teardown, as AntWorkout.py does)
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)


if __name__ == "__main__":
    main()
