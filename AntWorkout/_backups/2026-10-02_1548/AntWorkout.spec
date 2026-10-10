# -*- mode: python ; coding: utf-8 -*-
# Build:  py -3.10 -m PyInstaller --noconfirm AntWorkout.spec
# Output: dist\AntWorkout\ (zip the whole folder; run AntWorkout.exe or LayoutViewer.exe)
#
# Two exes, AntWorkout.exe and LayoutViewer.exe, sharing one _internal\ folder (Qt etc. only once).
# Bundled: ant_native.pyd + ant package, fonts, sounds, gauge svgs.
# Not bundled: .awo workouts, savedSettingsANT.json, defaultTheme.json, logs - these live next
# to the exes (both apps chdir there when frozen). themes\ (colors and gradients files) and
# awoWorkouts\ are copied next to them at the end of the build, so more can be added without a rebuild.


#AntWorkout.py and LayoutViewer.py add these to sys.path at runtime; PyInstaller needs them
#up front to find the ant package and the ant_native.pyd extension
PATHEX = ['AntNative\\python', 'AntNative\\build\\Release']
#ant_native.pyd loads before PyQt6 (see top of AntWorkout.py), so it can't
#borrow the C++ runtime from PyQt6\Qt6\bin; ship it next to the pyd so the
#target PC doesn't need the VC++ redistributable installed
BINARIES = [
    ('C:\\Windows\\System32\\msvcp140.dll', '.'),
    ('C:\\Windows\\System32\\vcruntime140.dll', '.'),
    ('C:\\Windows\\System32\\vcruntime140_1.dll', '.'),
]
DATAS = [
    ('fonts\\Orbitron-Regular.ttf', 'fonts'),
    ('fonts\\Orbitron-Bold.ttf', 'fonts'),
    ('fonts\\Orbitron-Black.ttf', 'fonts'),
    #variable fonts a theme can use (see AppTheme.py)
    ('fonts\\variable\\*.ttf', 'fonts\\variable'),
    ('sounds\\*.wav', 'sounds'),
    ('Chainring.svg', '.'),
    ('Heart.svg', '.'),
    #play/pause/stop button symbols (see AppLayout.py)
    ('Play.svg', '.'),
    ('Pause.svg', '.'),
    ('Stop.svg', '.'),
    ('HeartSmaller.svg', '.'),
]
HIDDENIMPORTS = ['ant_native', 'pyqtgraph', 'PyQt6.QtSvg', 'PyQt6.QtSvgWidgets', 'PyQt6.QtMultimedia',
                 'miniaudio', '_miniaudio', '_cffi_backend']
#other Qt bindings / plotting libs installed alongside, not used by the apps
EXCLUDES = ['PyQt5', 'PySide2', 'PySide6', 'shiboken2', 'shiboken6',
            'matplotlib', 'tkinter', 'pyqtgraph.opengl']

#PyQt6 bundles an old C++ runtime (MSVCP140 14.26) in PyQt6\Qt6\bin, and
#PyInstaller's PyQt6 hook puts that folder on the DLL path before AntWorkout.py
#runs. ant_native.pyd is built with VS 14.40+, whose std::mutex needs a 14.40+
#MSVCP140 - with Qt's copy it crashes (0xC0000005 in MSVCP140.dll). Swap every
#bundled copy for the system one (newer, and backward compatible for Qt).
import os
import shutil
_SYS32 = os.path.join(os.environ['SystemRoot'], 'System32')
_CRT = {'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
        'vcruntime140.dll', 'vcruntime140_1.dll'}


def system_crt(binaries):
    return [
        (dest, os.path.join(_SYS32, os.path.basename(dest)), kind)
        if os.path.basename(dest).lower() in _CRT
        and os.path.exists(os.path.join(_SYS32, os.path.basename(dest)))
        else (dest, src, kind)
        for dest, src, kind in binaries
    ]


def analysis(script):
    a = Analysis(
        [script],
        pathex=PATHEX,
        binaries=BINARIES,
        datas=DATAS,
        hiddenimports=HIDDENIMPORTS,
        hookspath=[],
        hooksconfig={},
        runtime_hooks=[],
        excludes=EXCLUDES,
        noarchive=False,
        optimize=0,
    )
    a.binaries = system_crt(a.binaries)
    return a


def exe(a, name):
    return EXE(
        PYZ(a.pure),
        a.scripts,
        [],
        exclude_binaries=True,
        name=name,
        debug=False,
        bootloader_ignore_signals=False,
        strip=False,
        upx=False,
        console=False,
        disable_windowed_traceback=False,
        argv_emulation=False,
        target_arch=None,
        codesign_identity=None,
        entitlements_file=None,
    )


workout = analysis('AntWorkout.py')
viewer = analysis('LayoutViewer.py')
workoutExe = exe(workout, 'AntWorkout')
viewerExe = exe(viewer, 'LayoutViewer')

#one folder for both: files the two have in common are only collected once
coll = COLLECT(
    workoutExe,
    workout.binaries,
    workout.datas,
    viewerExe,
    viewer.binaries,
    viewer.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='AntWorkout',
)

#the colors/gradients files and the workouts, next to the exes (see AppTheme.THEMES_DIR); COLLECT
#empties the output folder first, so these are copied fresh on every build
for folder in ('themes', 'awoWorkouts'):
    shutil.copytree(folder, os.path.join(DISTPATH, 'AntWorkout', folder), dirs_exist_ok=True)
