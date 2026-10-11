"""
MusicCadence.py

Musical Cadence Mode's music side: reading the songs picked (a folder, files, or an .m3u/.m3u8 playlist),
working out each song's tempo all the way through, and turning a playlist's tempos into a workout of
target cadences (RPM) rather than watts. No Qt in here, so the analysis can run in worker processes.

Tempo, per song (numpy only):
  1. decoded to mono at SAMPLE_RATE (miniaudio for mp3/flac/wav; plain wav via the wave module without it)
  2. onset envelope: spectral flux of the log magnitude spectrum, FPS frames a second
  3. every STEP_SECONDS, the envelope's autocorrelation over WINDOW_SECONDS; each candidate tempo's
     strength is summed over its octaves (half, double, quadruple), so a beat counted at half or double
     speed lands in the same "tempo class" - only which octave to ride at is left open, and that is what
     the 40-120 RPM folding decides anyway
  4. each window's class (octaves above 60 BPM, 0 to 1) and how clearly it stands out (confidence)
  5. silent stretches (SILENCE_MIN_SECONDS or longer): free ride, whatever the minimum segment
  6. the beats themselves, at that tempo: dynamic programming over the onset envelope (each beat as strong as it
     can be while the gaps between beats stay close to the local tempo), for the HUD's beat pulse

Playback (MusicPlayer) is miniaudio too: the ride's songs as one continuous stream, whose position (the audio
frames handed to the sound device) is the workout's clock, so the targets can't drift from the music.

That per-window track is cached (CACHE_FILE, keyed by file name, size and CRC32, under cache_version()), so a song is
only ever analyzed once; the segments are worked out from it each time, which is instant:
  - the classes are unwrapped into one continuous tempo line (so 119 -> 121 BPM doesn't flip to 60.5),
    median filtered, cut wherever the tempo moves more than SEGMENT_TOLERANCE, and anything shorter than
    the minimum segment is merged into its nearest neighbor
  - stretches with no clear beat (confidence under MIN_CONFIDENCE) become free ride, and so does silence (in a song,
    or a song's quiet lead in/out, which is the gap between songs)
  - each segment's tempo is doubled/halved into MIN_RPM..MAX_RPM, keeping to the previous segment's
    octave within a song (continuity), else the one nearest the preferred cadence
"""

import json
import os
import random
import threading
import time
import wave
import zlib
from concurrent.futures import ProcessPoolExecutor, as_completed

import numpy as np

try:
    import miniaudio
except ImportError: #wav only then
    miniaudio = None


AUDIO_EXTENSIONS = (".mp3", ".wav", ".flac")
PLAYLIST_EXTENSIONS = (".m3u", ".m3u8")

MIN_RPM, MAX_RPM = 40, 120
DEFAULT_PREFERRED_RPM = 85
DEFAULT_MIN_SEGMENT_SECONDS = 8

#bump when the analysis code changes, so cached results from the old one are redone (its settings below are checked
#by cache_version() themselves)
ANALYSIS_VERSION = 3
CACHE_FILE = "musicCadenceCacheANT.json"
#the song picker's list from last time: [[path, title, checked], ...]
SELECTION_FILE = "musicSelectionANT.json"

SAMPLE_RATE = 11025
N_FFT = 512
HOP = 128
FPS = SAMPLE_RATE/HOP #~86 envelope frames a second
WINDOW_SECONDS = 8.0
STEP_SECONDS = 2.0
#tempo classes: 60 to 120 BPM, CLASS_STEPS per octave
CLASS_STEPS = 120
#octaves each class is checked at: 30-60, 60-120, 120-240, 240-480 BPM
CLASS_OCTAVES = (-1, 0, 1, 2)
CLASS_OCTAVE_WEIGHTS = (0.5, 1.0, 1.0, 0.5)

#silence: SILENCE_STEP_SECONDS stretches quieter than SILENCE_DB (dBFS) and SILENCE_BELOW_LOUD dB under the song's
#loud parts, for at least SILENCE_MIN_SECONDS in a row
SILENCE_STEP_SECONDS = 0.5
SILENCE_DB = -50
SILENCE_BELOW_LOUD = 30
SILENCE_MIN_SECONDS = 3.0

#windows with less than this confidence have no clear beat
MIN_CONFIDENCE = 2.5
#a new segment starts when the tempo moves more than this (octaves, ~3%)
SEGMENT_TOLERANCE = 0.043
MEDIAN_WINDOWS = 5


###----------------------------------------------------------------------------------------------
### Picking songs
###----------------------------------------------------------------------------------------------
def is_audio(path):
    return path.lower().endswith(AUDIO_EXTENSIONS)


def is_playlist(path):
    return path.lower().endswith(PLAYLIST_EXTENSIONS)


def folder_songs(folder):
    """Every song in a folder and its subfolders, in name order"""
    songs = []
    for root, dirs, files in os.walk(folder):
        dirs.sort(key=str.lower)
        songs += [os.path.join(root, f) for f in sorted(files, key=str.lower) if is_audio(f)]
    return [os.path.normpath(s) for s in songs]


def read_playlist(path):
    """(songs, missing): an .m3u/.m3u8's songs that exist, and the entries that don't. Relative entries are
    from the playlist's folder; #EXTINF titles are kept as each song's title"""
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError:
        return [], [path]
    #.m3u8 is UTF-8; plain .m3u is usually UTF-8 too, else the Windows codepage
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = raw.decode("cp1252", errors="replace")
    base = os.path.dirname(os.path.abspath(path))
    songs, missing = [], []
    title = None
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("#"):
            if line.upper().startswith("#EXTINF:") and "," in line:
                title = line.split(",", 1)[1].strip() or None
            continue
        if line.lower().startswith("file:///"):
            line = line[8:]
        songPath = os.path.normpath(line if os.path.isabs(line) else os.path.join(base, line))
        if os.path.isfile(songPath) and is_audio(songPath):
            songs.append((songPath, title))
        else:
            missing.append(line)
        title = None
    return songs, missing


def song_title(path):
    return os.path.splitext(os.path.basename(path))[0]


def random_selection(songs, targetSeconds, durations, rng=None):
    """Songs picked at random (each at most once) to come as close to targetSeconds as they can: songs are
    added while they fit, then the leftover time is filled with whichever unused song comes closest"""
    rng = rng or random.Random()
    pool = [s for s in songs if durations.get(s)]
    rng.shuffle(pool)
    picked, total = [], 0.0
    for song in list(pool):
        if total + durations[song] <= targetSeconds:
            picked.append(song)
            pool.remove(song)
            total += durations[song]
    #one more song if it lands closer to the target than stopping short does
    if pool and total < targetSeconds:
        best = min(pool, key=lambda s: abs(total + durations[s] - targetSeconds))
        if abs(total + durations[best] - targetSeconds) < targetSeconds - total:
            picked.append(best)
    return picked


###----------------------------------------------------------------------------------------------
### Analysis (runs in worker processes)
###----------------------------------------------------------------------------------------------
def decode(path):
    """Mono float32 samples at SAMPLE_RATE"""
    if miniaudio is not None:
        #from the bytes rather than the path: miniaudio can't open some non-ASCII file names itself
        with open(path, "rb") as f:
            data = f.read()
        decoded = miniaudio.decode(data, output_format=miniaudio.SampleFormat.FLOAT32,
                                   nchannels=1, sample_rate=SAMPLE_RATE)
        return np.frombuffer(decoded.samples, dtype=np.float32)
    if not path.lower().endswith(".wav"):
        raise RuntimeError("mp3/flac need the miniaudio package")
    with wave.open(path, "rb") as w:
        channels, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
        data = w.readframes(w.getnframes())
    if width != 2:
        raise RuntimeError("only 16 bit wav files are supported without miniaudio")
    samples = np.frombuffer(data, dtype=np.int16).astype(np.float32).reshape(-1, channels).mean(axis=1)/32768
    #(crude) resample to SAMPLE_RATE
    count = int(len(samples)*SAMPLE_RATE/rate)
    return np.interp(np.linspace(0, len(samples) - 1, count), np.arange(len(samples)), samples).astype(np.float32)


def onset_envelope(samples):
    """Spectral flux of the log magnitude spectrum, one value per HOP samples"""
    if len(samples) < N_FFT:
        return np.zeros(0, dtype=np.float32)
    frames = np.lib.stride_tricks.sliding_window_view(samples, N_FFT)[::HOP]
    window = np.hanning(N_FFT).astype(np.float32)
    flux = np.empty(len(frames), dtype=np.float32)
    previous = None
    chunk = 8192 #frames at a time, keeps memory down on long files
    for start in range(0, len(frames), chunk):
        spectrum = np.log1p(100*np.abs(np.fft.rfft(frames[start:start+chunk]*window, axis=1))).astype(np.float32)
        if previous is None:
            previous = spectrum[:1]
        diffs = np.diff(np.concatenate((previous, spectrum)), axis=0)
        flux[start:start+len(spectrum)] = np.maximum(diffs, 0).sum(axis=1)
        previous = spectrum[-1:]
    #take out the slowly moving loudness so only the hits are left
    local = np.convolve(flux, np.ones(int(FPS/2))/int(FPS/2), mode="same")
    return np.maximum(flux - local, 0)


def _class_lags():
    """lags (frames) for every tempo class at each of its octaves: (octaves, classes)"""
    classes = 60*2**(np.arange(CLASS_STEPS)/CLASS_STEPS)
    return np.array([60*FPS/(classes*2**octave) for octave in CLASS_OCTAVES])


def tempo_track(envelope):
    """(classes, confidences) every STEP_SECONDS: each window's tempo class in octaves above 60 BPM (0 to 1),
    and how far its strongest class stands above the rest (in standard deviations)"""
    width = int(WINDOW_SECONDS*FPS)
    step = int(STEP_SECONDS*FPS)
    if len(envelope) < width:
        if len(envelope) < 2*FPS:
            return [], []
        width = len(envelope)
    lags = _class_lags()
    weights = np.array(CLASS_OCTAVE_WEIGHTS)[:, None]
    classes, confidences = [], []
    for start in range(0, len(envelope) - width + 1, step):
        segment = envelope[start:start+width]
        segment = segment - segment.mean()
        spectrum = np.fft.rfft(segment, n=2*width)
        acf = np.fft.irfft(np.abs(spectrum)**2)[:width]
        if acf[0] <= 0:
            classes.append(0.0)
            confidences.append(0.0)
            continue
        #unbiased, then normalized so acf[0] is 1; lightly smoothed so a slightly off lag still counts
        acf = acf/(width - np.arange(width))
        acf = np.convolve(acf/acf[0], (0.25, 0.5, 0.25), mode="same")
        valid = lags < width - 1
        strength = (np.interp(lags, np.arange(width), acf)*valid*weights).sum(axis=0)
        best = int(np.argmax(strength))
        spread = strength.std()
        #parabolic peak, wrapping around the octave
        left, right = strength[best - 1], strength[(best + 1) % CLASS_STEPS]
        denominator = left - 2*strength[best] + right
        offset = 0.5*(left - right)/denominator if denominator < 0 else 0.0
        classes.append(((best + offset)/CLASS_STEPS) % 1.0)
        confidences.append(float((strength[best] - strength.mean())/spread) if spread > 0 else 0.0)
    return classes, confidences


#how strongly the beat tracker keeps to the local tempo (higher: steadier beats, fewer jumps onto off beat hits)
BEAT_TIGHTNESS = 100


def track_beats(envelope, classes):
    """Envelope frames of the beats, at the tempo the windows' classes follow (60-120 BPM, the unwrapped line
    held to one octave so the beat level doesn't flip where the class wraps around)"""
    if not classes or len(envelope) < 2*FPS:
        return []
    line = _median_filter(_unwrap(np.array(classes, dtype=float)), MEDIAN_WINDOWS)
    line -= np.floor(np.median(line))
    centers = (np.arange(len(classes))*STEP_SECONDS + min(WINDOW_SECONDS, len(envelope)/FPS)/2)*FPS
    bpm = np.clip(60*2**np.interp(np.arange(len(envelope)), centers, line), 40, 240)
    periods = 60*FPS/bpm
    #a beat lands on (or just after) an onset: the envelope lightly smoothed
    local = np.convolve(envelope/(envelope.std() or 1), np.hanning(7)/np.hanning(7).sum(), mode="same")
    score = local.astype(float)
    backlink = np.full(len(local), -1)
    penalties = {}
    for t in range(len(local)):
        period = int(round(periods[t]))
        if period not in penalties:
            gaps = np.arange(max(1, period//2), 2*period + 1)
            penalties[period] = (gaps, -BEAT_TIGHTNESS*np.log(gaps/period)**2)
        gaps, penalty = penalties[period]
        usable = gaps <= t
        if not usable.any():
            continue
        candidates = score[t - gaps[usable]] + penalty[usable]
        best = int(np.argmax(candidates))
        score[t] = local[t] + candidates[best]
        backlink[t] = t - gaps[usable][best]
    #the last beat: the best scoring frame in the final period, then back through the chain
    tail = int(round(periods[-1]))
    beat = len(score) - tail + int(np.argmax(score[-tail:]))
    beats = []
    while beat >= 0:
        beats.append(beat)
        beat = backlink[beat]
    return beats[::-1]


def silences(samples):
    """[(start, end)] seconds of each silent stretch at least SILENCE_MIN_SECONDS long"""
    step = int(SILENCE_STEP_SECONDS*SAMPLE_RATE)
    count = len(samples)//step
    if count == 0:
        return []
    rms = np.sqrt(np.mean(samples[:count*step].reshape(count, step).astype(np.float64)**2, axis=1))
    db = 20*np.log10(np.maximum(rms, 1e-9))
    quiet = db < min(SILENCE_DB, np.percentile(db, 95) - SILENCE_BELOW_LOUD)
    stretches = []
    i = 0
    while i < count:
        if quiet[i]:
            j = i
            while j < count and quiet[j]:
                j += 1
            #(a song's last partial step goes with its last whole one)
            end = len(samples)/SAMPLE_RATE if j == count else j*SILENCE_STEP_SECONDS
            if end - i*SILENCE_STEP_SECONDS >= SILENCE_MIN_SECONDS:
                stretches.append((round(i*SILENCE_STEP_SECONDS, 2), round(end, 2)))
            i = j
        else:
            i += 1
    return stretches


def beat_seconds(analysis):
    """A cached analysis' beats in seconds into the song"""
    frames = np.cumsum(analysis.get("beats", []))
    return (frames*HOP + N_FFT/2)/SAMPLE_RATE


def analyze_file(path):
    """The cacheable analysis of one song: {"duration", "classes", "confidences", "beats"}, or {"error"}. beats are
    envelope frames, each as the gap from the one before (short in the cache)"""
    try:
        samples = decode(path)
        envelope = onset_envelope(samples)
        classes, confidences = tempo_track(envelope)
        beats = track_beats(envelope, classes)
        return {"duration": len(samples)/SAMPLE_RATE, "classes": [round(c, 4) for c in classes],
                "confidences": [round(c, 2) for c in confidences],
                "beats": [int(b) for b in np.diff(beats, prepend=0)], "silences": silences(samples)}
    except Exception as e: #a corrupt or unreadable file just can't be used
        return {"error": f"{type(e).__name__}: {e}"}


###----------------------------------------------------------------------------------------------
### Cache
###----------------------------------------------------------------------------------------------
def cache_version():
    """What the cached analyses were made with: ANALYSIS_VERSION and the settings the analysis uses, so changing
    either one redoes them all. (The minimum segment, preferred cadence, MIN_CONFIDENCE and SEGMENT_TOLERANCE are
    applied to the cached analysis each time, so changing those needs nothing redone)"""
    return {"analysis": ANALYSIS_VERSION, "sampleRate": SAMPLE_RATE, "fft": N_FFT, "hop": HOP,
            "window": WINDOW_SECONDS, "step": STEP_SECONDS, "classSteps": CLASS_STEPS,
            "classOctaves": list(CLASS_OCTAVES), "classOctaveWeights": list(CLASS_OCTAVE_WEIGHTS),
            "medianWindows": MEDIAN_WINDOWS, "beatTightness": BEAT_TIGHTNESS, "silenceStep": SILENCE_STEP_SECONDS,
            "silenceDb": SILENCE_DB, "silenceBelowLoud": SILENCE_BELOW_LOUD, "silenceMin": SILENCE_MIN_SECONDS}


#{(path, size, modified ns): crc}, so a song's file is only read for its CRC once a run
_crcs = {}


def _crc(path, stat):
    known = (os.path.normcase(os.path.abspath(path)), stat.st_size, stat.st_mtime_ns)
    if known not in _crcs:
        crc = 0
        with open(path, "rb") as f:
            while chunk := f.read(1 << 20):
                crc = zlib.crc32(chunk, crc)
        _crcs[known] = crc
    return _crcs[known]


def _cache_key(path):
    """<file name>|<size>|<CRC32>: a song renamed, replaced or changed is a different song; one moved to another
    folder is the same"""
    stat = os.stat(path)
    return f"{os.path.normcase(os.path.basename(path))}|{stat.st_size}|{_crc(path, stat):08x}"


def load_cache():
    try:
        with open(CACHE_FILE, encoding="utf-8") as f:
            cache = json.load(f)
        if cache.get("version") == cache_version() and isinstance(cache.get("songs"), dict):
            return cache
    except (OSError, ValueError):
        pass
    return {"version": cache_version(), "songs": {}}


def save_cache(cache):
    temp = CACHE_FILE + ".tmp"
    with open(temp, "w", encoding="utf-8") as f:
        json.dump(cache, f, separators=(",", ":"))
    os.replace(temp, CACHE_FILE)


def cached(cache, path):
    try:
        return cache["songs"].get(_cache_key(path))
    except OSError:
        return None


def analyze_songs(paths, progress=None, cancelled=None, workers=None):
    """{path: analysis} for every song, from the cache where it can be and worker processes otherwise.
    progress(done, total) after each song; cancelled() returning True stops early (what's done is kept)"""
    cache = load_cache()
    results, todo = {}, []
    for path in dict.fromkeys(paths):
        hit = cached(cache, path)
        if hit is not None:
            results[path] = hit
        else:
            todo.append(path)
    total, done = len(results) + len(todo), len(results)
    if progress:
        progress(done, total)
    if todo:
        workers = workers or max(1, min(len(todo), (os.cpu_count() or 2) - 1))
        try:
            with ProcessPoolExecutor(max_workers=workers) as pool:
                futures = {pool.submit(analyze_file, path): path for path in todo}
                for future in as_completed(futures):
                    path = futures[future]
                    results[path] = future.result()
                    if "error" not in results[path]:
                        try:
                            cache["songs"][_cache_key(path)] = results[path]
                        except OSError:
                            pass
                    done += 1
                    if progress:
                        progress(done, total)
                    if cancelled and cancelled():
                        for other in futures:
                            other.cancel()
                        break
        finally:
            save_cache(cache)
    return results


def load_selection():
    try:
        with open(SELECTION_FILE, encoding="utf-8") as f:
            songs = json.load(f)
        return [s for s in songs if isinstance(s, list) and len(s) == 3]
    except (OSError, ValueError):
        return []


def save_selection(songs):
    temp = SELECTION_FILE + ".tmp"
    try:
        with open(temp, "w", encoding="utf-8") as f:
            json.dump(songs, f)
        os.replace(temp, SELECTION_FILE)
    except OSError:
        pass


###----------------------------------------------------------------------------------------------
### Segments and the workout
###----------------------------------------------------------------------------------------------
def _unwrap(classes):
    """Tempo classes (mod 1 octave) as one continuous line: each the octave closest to the one before"""
    line = []
    for c in classes:
        if line:
            c += round(line[-1] - c)
        line.append(c)
    return np.array(line)


def _median_filter(values, width):
    half = width//2
    padded = np.concatenate((np.full(half, values[0]), values, np.full(half, values[-1])))
    return np.array([np.median(padded[i:i+width]) for i in range(len(values))])


def song_segments(analysis, minSegmentSeconds=DEFAULT_MIN_SEGMENT_SECONDS):
    """[(start, end, bpm or None)] covering the whole song, in seconds; None is no clear beat (free ride).
    bpm is somewhere in the 60-120 octave"""
    duration = analysis["duration"]
    classes, confidences = analysis["classes"], analysis["confidences"]
    if not classes:
        return [(0.0, duration, None)]
    beat = np.array(confidences) >= MIN_CONFIDENCE
    #each window speaks for the time around its center
    centers = np.arange(len(classes))*STEP_SECONDS + min(WINDOW_SECONDS, duration)/2
    minWindows = max(1, int(round(minSegmentSeconds/STEP_SECONDS)))

    #runs of beat/no beat; no beat runs shorter than the minimum are just bridged over
    labels = list(beat)
    _bridge_short_runs(labels, False, minWindows)
    if not any(labels):
        return [(0.0, duration, None)]
    #the tempo line through the beat windows only, filled across the gaps from the nearest beat window
    index = np.arange(len(classes))
    beatIndex = index[beat]
    nearest = beatIndex[np.abs(index[:, None] - beatIndex[None, :]).argmin(axis=1)]
    line = _median_filter(_unwrap(np.array(classes)[nearest]), MEDIAN_WINDOWS)

    #cut into segments: a new one whenever the line leaves the running segment's tempo, and at beat changes
    pieces = [] #[first window, last window, is beat]
    for i in range(len(classes)):
        if pieces and pieces[-1][2] == labels[i] and \
                (not labels[i] or abs(line[i] - np.median(line[pieces[-1][0]:i])) <= SEGMENT_TOLERANCE):
            pieces[-1][1] = i
        else:
            pieces.append([i, i, labels[i]])
    #short pieces go to whichever neighbor is closest in tempo (a short no beat piece: to its longer neighbor)
    while len(pieces) > 1:
        lengths = [p[1] - p[0] + 1 for p in pieces]
        shortest = min(range(len(pieces)), key=lambda k: lengths[k])
        if lengths[shortest] >= minWindows:
            break
        neighbors = [k for k in (shortest - 1, shortest + 1) if 0 <= k < len(pieces)]
        def distance(k):
            if pieces[k][2] != pieces[shortest][2]:
                return 10 + (1/lengths[k])
            return abs(np.median(line[pieces[k][0]:pieces[k][1]+1]) -
                       np.median(line[pieces[shortest][0]:pieces[shortest][1]+1]))
        target = min(neighbors, key=distance)
        #the joined piece is beat or not as the longer of the two was
        keepBeat = pieces[target][2] if lengths[target] >= lengths[shortest] else pieces[shortest][2]
        low, high = sorted((shortest, target))
        pieces[low:high+1] = [[pieces[low][0], pieces[high][1], keepBeat]]
    #neighbors that ended up within the tolerance of each other join up
    def tempo(piece):
        return np.median(line[piece[0]:piece[1]+1])
    k = 0
    while k < len(pieces) - 1:
        a, b = pieces[k], pieces[k + 1]
        if a[2] == b[2] and (not a[2] or abs(tempo(a) - tempo(b)) <= SEGMENT_TOLERANCE):
            pieces[k:k+2] = [[a[0], b[1], a[2]]]
        else:
            k += 1
    segments = []
    for k, (first, last, isBeat) in enumerate(pieces):
        start = 0.0 if k == 0 else (centers[first - 1] + centers[first])/2
        end = duration if k == len(pieces) - 1 else (centers[last] + centers[last + 1])/2
        bpm = 60*2**(float(np.median(line[first:last+1])) % 1.0) if isBeat else None
        segments.append((start, end, bpm))
    return _with_silences(segments, analysis.get("silences", []))


def _with_silences(segments, silentStretches):
    """The segments with each silent stretch cut out of them as free ride (None); leftovers shorter than a silence
    (SILENCE_MIN_SECONDS) go to their neighbor, and neighbors with the same tempo (free ride with free ride) join"""
    pieces = []
    for start, end, bpm in segments:
        cuts = [(max(s, start), min(e, end)) for s, e in silentStretches if s < end and e > start]
        position = start
        for cutStart, cutEnd in cuts:
            if cutStart > position:
                pieces.append([position, cutStart, bpm])
            pieces.append([cutStart, cutEnd, None])
            position = cutEnd
        if position < end:
            pieces.append([position, end, bpm])
    joined = []
    for piece in pieces:
        if joined and (piece[1] - piece[0] < SILENCE_MIN_SECONDS or joined[-1][2] == piece[2]):
            joined[-1][1] = piece[1]
        elif joined and joined[-1][1] - joined[-1][0] < SILENCE_MIN_SECONDS:
            joined[-1][1:] = piece[1:]
        else:
            joined.append(piece)
    return [tuple(piece) for piece in joined]


def _bridge_short_runs(labels, value, minLength):
    """Runs of value shorter than minLength flip to the other value (in place)"""
    i = 0
    while i < len(labels):
        if labels[i] == value:
            j = i
            while j < len(labels) and labels[j] == value:
                j += 1
            if j - i < minLength:
                labels[i:j] = [not value]*(j - i)
            i = j
        else:
            i += 1


def fold_rpm(bpm, previous=None, preferred=DEFAULT_PREFERRED_RPM, low=MIN_RPM, high=MAX_RPM):
    """bpm doubled/halved into low..high: the octave nearest previous (the segment before in the same
    song) when there is one, else nearest preferred"""
    options = []
    value = bpm
    while value > high:
        value /= 2
    while value*2 <= high:
        value *= 2
    while value >= low:
        options.append(value)
        value /= 2
    if not options: #(only if high < 2*low)
        options = [min(max(bpm, low), high)]
    anchor = previous or preferred
    return min(options, key=lambda v: abs(np.log2(v/anchor)))


def _pulses(beats, start, end, rpm):
    """The beats from start to end (seconds) as one pulse per pedal stroke at rpm: as they are, halved (every other
    beat), doubled (a pulse halfway between beats too) or further, whichever brings their pace closest to rpm"""
    inside = beats[(beats >= start) & (beats < end)]
    if len(inside) < 2 or not rpm:
        return []
    beatRpm = 60/np.median(np.diff(inside))
    octaves = int(round(np.log2(rpm/beatRpm)))
    if octaves < 0:
        return list(inside[::2**-octaves])
    pulses = list(inside)
    for _ in range(octaves):
        pulses = [p for pair in zip(pulses, (np.array(pulses[1:]) + np.array(pulses[:-1]))/2) for p in pair] + pulses[-1:]
    return pulses


def build_workout(songs, analyses, preferred=DEFAULT_PREFERRED_RPM, minSegmentSeconds=DEFAULT_MIN_SEGMENT_SECONDS):
    """The playlist's ride: (blocks, tracks, songStarts, pulses)
      blocks:     [(duration seconds (int), rpm or 0 for free ride)], neighbors in the same song with the same rpm
                  joined; block edges are at whole seconds of playlist time, so durations add up to the rounded
                  playlist length
      tracks:     [(path, title, start, duration)] in playlist time (exact, for the music clock)
      songStarts: {block index: track index} for each song's first block
      pulses:     the beat pulse times (playlist seconds), one per pedal stroke at each segment's cadence"""
    blocks, tracks, songStarts, pulses = [], [], {}, []
    start = 0.0
    edge = 0
    for path, title in songs:
        analysis = analyses.get(path)
        if not analysis or "error" in analysis or analysis["duration"] <= 0:
            continue
        trackIndex = len(tracks)
        tracks.append((path, title or song_title(path), start, analysis["duration"]))
        beats = beat_seconds(analysis)
        previous = None
        firstBlock = True
        for segStart, segEnd, bpm in song_segments(analysis, minSegmentSeconds):
            rpm = 0
            if bpm is not None:
                rpm = int(round(fold_rpm(bpm, previous, preferred)))
                previous = rpm
                pulses += [start + p for p in _pulses(beats, segStart, segEnd, rpm)]
            roundedEnd = int(round(start + segEnd))
            if roundedEnd <= edge:
                continue
            if not firstBlock and blocks[-1][1] == rpm:
                blocks[-1] = (blocks[-1][0] + roundedEnd - edge, rpm)
            else:
                if firstBlock:
                    songStarts[len(blocks)] = trackIndex
                    firstBlock = False
                blocks.append((roundedEnd - edge, rpm))
            edge = roundedEnd
        start += analysis["duration"]
    return blocks, tracks, songStarts, pulses


def song_header(track):
    """A song's line in the workout lists: <length> : <title> (the title is usually "<Artist> - <Title>")"""
    _, title, _, duration = track
    from WorkoutHelpers import MinimizedDuration
    return MinimizedDuration(int(round(duration))) + " : " + title


###----------------------------------------------------------------------------------------------
### Playback
###----------------------------------------------------------------------------------------------
class MusicPlayer:
    """Plays the ride's songs back to back on the default sound device, on miniaudio's own audio thread.
    tracks: [(path, title, start, duration)] as build_workout() gives them. position() is where the music is in the
    ride (each song's start plus the frames of it played so far), the workout's clock. A song that can't be read
    is skipped: position() jumps to the next song's start with it"""
    SAMPLE_RATE = 44100
    CHANNELS = 2
    BYTES_PER_FRAME = 2*CHANNELS #signed 16 bit

    def __init__(self, tracks):
        self.tracks = list(tracks)
        self.track = 0
        self.trackFrames = 0
        self.paused = False
        self.finished = False
        self.volume = 1.0 #0 to 1
        self.skipped = []
        #every frame handed to the device (silence too) and when the first was: what's been handed but not heard
        #yet (the device's buffer, a few hundred ms) is taken off position()
        self.framesHanded = 0
        self.firstCallback = None #(path, reason) of songs that couldn't be played
        self.device = None
        self._lock = threading.Lock()

    @staticmethod
    def available():
        return miniaudio is not None

    def start(self):
        """Starts playing from the first song; raises if there's no sound device (or no miniaudio)"""
        if miniaudio is None:
            raise RuntimeError("the miniaudio package is needed to play music")
        self.device = miniaudio.PlaybackDevice(output_format=miniaudio.SampleFormat.SIGNED16, nchannels=self.CHANNELS,
                                               sample_rate=self.SAMPLE_RATE, buffersize_msec=100,
                                               app_name="Awooo Workout")
        stream = self._stream()
        next(stream)
        self.device.start(stream)

    def stop(self):
        if self.device is not None:
            self.device.close()
            self.device = None

    def latency(self):
        """Seconds of audio handed to the device that haven't been heard yet"""
        if self.firstCallback is None:
            return 0.0
        return min(max(0.0, self.framesHanded/self.SAMPLE_RATE - (time.monotonic() - self.firstCallback)), 1.0)

    def position(self):
        """Seconds into the ride that are being heard now (past the end once the last song is done)"""
        with self._lock:
            if self.finished:
                return sum(duration for _, _, _, duration in self.tracks) + 1
            if self.track >= len(self.tracks):
                return 0.0
            _, _, start, duration = self.tracks[self.track]
            #(no further than the song's analyzed length, so a slightly long file can't run into the next song's time;
            #less than the song's start while the end of the one before is still being heard)
            handed = start + min(self.trackFrames/self.SAMPLE_RATE, duration)
            #(silence keeps the device's buffer just as full while paused, so this holds still then)
            return max(0.0, handed - self.latency())

    def _songs(self):
        """Each song's decoded chunks, song by song; marks the song being decoded"""
        for index, (path, _, _, _) in enumerate(self.tracks):
            try:
                with open(path, "rb") as f:
                    data = f.read()
                chunks = miniaudio.stream_memory(data, output_format=miniaudio.SampleFormat.SIGNED16,
                                                 nchannels=self.CHANNELS, sample_rate=self.SAMPLE_RATE,
                                                 frames_to_read=4096)
                first = next(chunks)
            except Exception as e: #unreadable: on to the next song
                self.skipped.append((path, f"{type(e).__name__}: {e}"))
                continue
            yield index, first
            for chunk in chunks:
                yield index, chunk

    def _stream(self):
        """The device's callback generator: sent how many frames it needs, yields that many (silence while paused)"""
        songs = self._songs()
        buffer = bytearray()
        required = yield b""
        while True:
            if self.firstCallback is None:
                self.firstCallback = time.monotonic()
            self.framesHanded += required
            needed = required*self.BYTES_PER_FRAME
            out = bytearray()
            while not (self.paused or self.finished) and len(out) < needed:
                if not buffer:
                    try:
                        index, chunk = next(songs)
                    except StopIteration:
                        with self._lock:
                            self.finished = True
                        break
                    if index != self.track:
                        #the next song (or a later one, past any that were skipped) starts here
                        with self._lock:
                            self.track, self.trackFrames = index, 0
                    buffer += chunk.tobytes()
                    continue
                take = buffer[:needed - len(out)]
                del buffer[:len(take)]
                out += take
                with self._lock:
                    self.trackFrames += len(take)//self.BYTES_PER_FRAME
            out += bytes(needed - len(out))
            if self.volume != 1.0:
                out = (np.frombuffer(bytes(out), dtype=np.int16)*max(0.0, min(self.volume, 1.0))).astype(np.int16).tobytes()
            required = yield bytes(out)
