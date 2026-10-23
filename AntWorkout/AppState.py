from PyQt6.QtGui import QBrush
from PyQt6.QtWidgets import QSizePolicy


class AppState:
    #Initialize FTP and Max HR
    ftp = "0"
    ftpRanges = []
    maxHr = "0"

    #Bike metrics
    wheelCircumference = "0" #in meters

    #Set default, workout mode
    workoutMode = True
    #an FTP test (a key of FtpTest.TESTS: "20m", "1h", "ramp10" or "ramp20") rather than a workout file; workoutMode is True for these too
    ftpTestMode = None
    #the FTP test's effort as ridden, set by the HUD: {"average": <watts, None before any reading>, "completed": <bool>}
    ftpTestResult = None
    #Musical Cadence Mode: the workout's targets are cadences (RPM) worked out from music, which plays while
    #riding (see MusicCadence.py); workoutMode is True for it too. The workout list's "power" values hold the
    #RPM, and ftpRanges holds the cadence zones while it's selected (see WorkoutSetupWindow.apply_cadence_ranges)
    cadenceMode = False
    #the ride's songs as (path, title, start second, duration) in workout time, for the HUD's music player
    musicTracks = []
    #{workoutList index: the song's header line} for each song's first block, shown before it in the workout lists
    musicSongStarts = {}
    #each song as it was heard: [musicTracks index, start, end] in AppState.secondsPassed, for the synopsis
    musicPlayed = []
    #beat pulse times (workout seconds), one per pedal stroke, for the cadence gauge's pulse
    musicPulses = []

    #Workout details
    workoutFilename = ""
    workoutFilenameShort = ""
    workoutTitle = "" #from an optional T:<title> line in the .awo file, shown in place of the filename
    workoutDurationSeconds = 0

    #List of Workout Blocks
    workoutList = []

    #Timed workout messages; each element is (<start second>, <end second>, <text>) in workout time
    workoutMessages = []

    #Name shown for the loaded workout: its title if the file has one, otherwise its filename
    @staticmethod
    def workout_display_name():
        return AppState.workoutTitle or AppState.workoutFilenameShort

    #Global Timer
    secondsPassed = 0.0

    #Time of workout power requested; ([secondsPassed],[power],[system time])
    workoutPowerRequestsSeconds = []
    workoutPowerRequestsWatts = []
    workoutPowerRequestsSysTime = []

    #Each graded block's grade (see WorkoutGrading.py), keyed (workoutList index, interval sub-block index, rep);
    #non-interval blocks are (index, 0, 0). Each is BlockGrader.finish()'s dict plus "label" (the block's text as
    #ridden), "start"/"end" (AppState.secondsPassed) and "top" (its highest target power)
    blockGrades = {}

    #The session's rides (see Session.py): each ride as it was, from when the app opened or Clear Session was
    #pressed, for the synopsis and the HUD's spiral ring
    sessionRides = []
    #what the ride being ridden keeps from when START was pressed (Session.begin_ride), None before any
    rideStart = None
    #the ride being ridden has joined the session (its synopsis opened)
    rideRecorded = False

    #Dark red used by the power/hr/cadence gauge needles and the power gauge's top zone;
    #also used for low battery warnings and the active FTP slider handle
    gaugeRed = "#8B0000"

    #Graph workout brushes
    graphWorkoutBrushDark = QBrush()
    graphWorkoutBrushMid = QBrush()
    graphWorkoutBrushLight = QBrush()

    #ANT Devices in use
    antHRM = False
    antSpeedCadence = False
    antCadence = False
    antSpeed = False

    #True when any speed and/or cadence source is selected (combined 121, or discrete 122/123)
    @staticmethod
    def any_speed_cadence():
        return AppState.antSpeedCadence or AppState.antCadence or AppState.antSpeed

    #True when the HUD has a cadence gauge: a speed/cadence source, or Musical Cadence Mode (which falls back
    #to the power meter's own cadence without one)
    @staticmethod
    def shows_cadence():
        return AppState.any_speed_cadence() or (AppState.workoutMode and AppState.cadenceMode)

    #"RPM" in Musical Cadence Mode, else "Watts": the unit of the workout's targets
    @staticmethod
    def target_unit():
        return "RPM" if AppState.cadenceMode and AppState.workoutMode else "Watts"

    #True when a real speed sensor is connected (combined 121 or discrete 123)
    @staticmethod
    def has_speed_sensor():
        return AppState.antSpeedCadence or AppState.antSpeed

    #True when a real cadence sensor is connected (combined 121 or discrete 122)
    @staticmethod
    def has_cadence_sensor():
        return AppState.antSpeedCadence or AppState.antCadence

    #Upper end of the free mode Watts slider when the power device is an FE-C trainer, set on the setup screen
    maxWatts = 1200

    #Workout mode with an FE-C trainer: while the workout asks for more than this many watts the trainer is
    #released (not held at the requested power); reset to 120% of FTP whenever FTP is set, saved like Max Watts
    powerDriveCutoff = 0

    #HUD specific stored metrics
    savedPowerVals = ([],[]) #each element is [<time string>,<power string>]
    savedHRVals = ([],[]) #each element is [<time string>,<hr string>]
    savedCadenceVals = ([],[]) #each element is [<time string>,<cadence integer>]
    totalRevolutions = 0 #convert to distance by multiplying by wheel circumference and either miles/meter or km/meter

    #Start time; when the HUD screen is entered before starting the workout
    startTimestamp = ""

    #The system default font from before Orbitron became the app-wide font (set in AntWorkout.py),
    #for the places that keep the old look: buttons and the synopsis graph axes
    defaultFont = None

    #Graphing settings
    graphSecondsShown = 720
    graphBreakpoint = 240

    retainSize = QSizePolicy()
    retainSize.setRetainSizeWhenHidden(True)
