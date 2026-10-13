"""MusicCadence: picking songs, the tempo analysis (on generated click tracks), the cache and the ride it builds"""

import json
import os
import random
import wave

import numpy as np
import pytest

import MusicCadence as M


def click_track(path, bpm, seconds, rate=22050, silence=None, seed=1):
    """A 16 bit mono wav of a click every beat (a decaying noise burst); silence=(start, end) seconds left silent"""
    rng = np.random.default_rng(seed)
    samples = np.zeros(int(seconds*rate), dtype=np.float64)
    burst = rng.standard_normal(int(0.03*rate))*np.exp(-np.linspace(0, 8, int(0.03*rate)))
    beat = 60.0/bpm
    t = 0.0
    while t < seconds:
        start = int(t*rate)
        samples[start:start + len(burst)] += 0.6*burst[:len(samples) - start]
        t += beat
    samples += 0.002*rng.standard_normal(len(samples))
    if silence:
        samples[int(silence[0]*rate):int(silence[1]*rate)] = 0
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((np.clip(samples, -1, 1)*32767).astype(np.int16).tobytes())
    return path


@pytest.fixture(scope="module")
def songs(tmp_path_factory):
    folder = tmp_path_factory.mktemp("songs")
    paths = {
        "100": click_track(str(folder / "a_100.wav"), 100, 40),
        "75": click_track(str(folder / "b_75.wav"), 75, 40, seed=2),
        "gap": click_track(str(folder / "c_gap.wav"), 100, 40, silence=(15, 24), seed=3),
    }
    analyses = {name: M.analyze_file(path) for name, path in paths.items()}
    return paths, analyses


###Picking songs
def test_audio_and_playlist_names():
    assert M.is_audio("a.MP3") and M.is_audio("b.flac") and M.is_audio("c.wav") and not M.is_audio("d.ogg")
    assert M.is_playlist("x.M3U") and M.is_playlist("y.m3u8") and not M.is_playlist("z.pls")
    assert M.song_title(os.path.join("music", "Artist - Song.mp3")) == "Artist - Song"


def test_folder_songs(tmp_path):
    (tmp_path / "B").mkdir()
    (tmp_path / "a").mkdir()
    for name in ("B/2.mp3", "B/1.wav", "a/z.flac", "top.mp3", "notes.txt"):
        (tmp_path / name).write_bytes(b"")
    found = [os.path.relpath(p, tmp_path).replace("\\", "/") for p in M.folder_songs(str(tmp_path))]
    assert found == ["top.mp3", "a/z.flac", "B/1.wav", "B/2.mp3"]


def test_read_playlist(tmp_path):
    (tmp_path / "music").mkdir()
    (tmp_path / "music" / "one.mp3").write_bytes(b"")
    (tmp_path / "two.wav").write_bytes(b"")
    absolute = str(tmp_path / "two.wav")
    playlist = tmp_path / "list.m3u8"
    playlist.write_text("#EXTM3U\n#EXTINF:123,Band - One\nmusic/one.mp3\n\n"
                        f"file:///{absolute}\nmissing.mp3\nnotes.txt\n", encoding="utf-8-sig")
    (tmp_path / "notes.txt").write_text("x")
    songs, missing = M.read_playlist(str(playlist))
    assert songs == [(os.path.normpath(str(tmp_path / "music" / "one.mp3")), "Band - One"),
                     (os.path.normpath(absolute), None)]
    assert missing == ["missing.mp3", "notes.txt"]


def test_read_playlist_codepage_and_missing_file(tmp_path):
    (tmp_path / "café.mp3").write_bytes(b"")
    playlist = tmp_path / "old.m3u"
    playlist.write_bytes("café.mp3\n".encode("cp1252"))
    songs, missing = M.read_playlist(str(playlist))
    assert [os.path.basename(s) for s, _ in songs] == ["café.mp3"]
    assert M.read_playlist(str(tmp_path / "none.m3u")) == ([], [str(tmp_path / "none.m3u")])


def test_random_selection():
    durations = {"a": 100, "b": 200, "c": 300, "d": 400, "none": 0}
    picked = M.random_selection(list(durations), 600, durations, random.Random(4))
    assert len(set(picked)) == len(picked) and "none" not in picked
    total = sum(durations[s] for s in picked)
    #as close as it can get
    assert abs(total - 600) <= 100
    assert M.random_selection(["a"], 60, durations, random.Random(1)) == ["a"]   #(over, but closer than nothing)
    assert M.random_selection(["a"], 50, durations, random.Random(1)) == []      #(a tie: stopping short)
    assert M.random_selection(["d"], 100, durations, random.Random(1)) == []


###Analysis
@pytest.mark.parametrize("name, bpm", [("100", 100), ("75", 75)])
def test_tempo_of_a_click_track(songs, name, bpm):
    paths, analyses = songs
    analysis = analyses[name]
    assert "error" not in analysis
    assert analysis["duration"] == pytest.approx(40, abs=0.01)
    segments = M.song_segments(analysis)
    assert len(segments) == 1
    start, end, found = segments[0]
    assert (start, end) == (0.0, pytest.approx(40, abs=0.01))
    #the tempo class is per octave: 75 BPM and 150 BPM are the same class
    assert found == pytest.approx(bpm, rel=0.03)


def test_beats_of_a_click_track(songs):
    paths, analyses = songs
    beats = M.beat_seconds(analyses["100"])
    gaps = np.diff(beats)
    assert np.median(gaps) == pytest.approx(0.6, rel=0.03) or np.median(gaps) == pytest.approx(0.3, rel=0.03)
    assert beats[0] < 1.0 and beats[-1] > 38


def test_silence_becomes_free_ride(songs):
    paths, analyses = songs
    analysis = analyses["gap"]
    assert any(start <= 15.5 and end >= 23.5 for start, end in analysis["silences"])
    segments = M.song_segments(analysis)
    assert [bpm is None for _, _, bpm in segments] == [False, True, False]
    assert segments[1][0] == pytest.approx(15, abs=0.6) and segments[1][1] == pytest.approx(24, abs=0.6)


def test_silences():
    rate = M.SAMPLE_RATE
    samples = np.concatenate([np.full(5*rate, 0.3), np.zeros(4*rate), np.full(5*rate, 0.3), np.zeros(2*rate)])
    #(found to the nearest step, SILENCE_STEP_SECONDS)
    (start, end), = M.silences(samples.astype(np.float32))
    assert start == pytest.approx(5.0, abs=M.SILENCE_STEP_SECONDS) and end == pytest.approx(9.0, abs=0.01)
    assert M.silences(np.zeros(10, dtype=np.float32)) == []


def test_decode_without_miniaudio(tmp_path, monkeypatch):
    path = click_track(str(tmp_path / "x.wav"), 90, 3, rate=22050)
    with_miniaudio = M.decode(path)
    monkeypatch.setattr(M, "miniaudio", None)
    without = M.decode(path)
    assert len(without) == pytest.approx(3*M.SAMPLE_RATE, abs=2)
    assert len(with_miniaudio) == pytest.approx(len(without), abs=2)
    with pytest.raises(RuntimeError):
        M.decode(str(tmp_path / "x.mp3"))


def test_unreadable_song_is_an_error(tmp_path):
    bad = tmp_path / "bad.mp3"
    bad.write_bytes(b"not audio")
    assert "error" in M.analyze_file(str(bad))


def test_tiny_inputs():
    assert len(M.onset_envelope(np.zeros(10, dtype=np.float32))) == 0
    assert M.tempo_track(np.zeros(10)) == ([], [])
    assert M.track_beats(np.zeros(10), []) == []


###Segments from an analysis
def analysis(classes, confidences=None, duration=None, silences=()):
    confidences = confidences or [5.0]*len(classes)
    duration = duration or len(classes)*M.STEP_SECONDS + M.WINDOW_SECONDS - M.STEP_SECONDS
    return {"duration": duration, "classes": classes, "confidences": confidences, "beats": [], "silences": list(silences)}


def test_segments_tempo_change():
    a = analysis([0.2]*15 + [0.6]*15)
    segments = M.song_segments(a)
    assert len(segments) == 2
    assert segments[0][2] == pytest.approx(60*2**0.2) and segments[1][2] == pytest.approx(60*2**0.6)
    assert segments[0][1] == segments[1][0] and segments[-1][1] == a["duration"]


def test_segments_wrap_around_the_octave_is_one_tempo():
    #119 BPM and 121 BPM are 0.99 and 0.01 octaves above 60: one steady tempo
    segments = M.song_segments(analysis([0.99, 0.005, 0.99, 0.005]*6))
    assert len(segments) == 1


def test_segments_short_changes_are_merged():
    segments = M.song_segments(analysis([0.2]*10 + [0.6]*2 + [0.2]*10), minSegmentSeconds=8)
    assert len(segments) == 1


def test_segments_no_clear_beat():
    assert M.song_segments(analysis([0.3]*10, [0.5]*10)) == [(0.0, analysis([0.3]*10)["duration"], None)]
    assert M.song_segments({"duration": 30, "classes": [], "confidences": []}) == [(0.0, 30, None)]
    #a short unclear stretch is bridged over
    segments = M.song_segments(analysis([0.3]*20, [5.0]*8 + [0.5]*2 + [5.0]*10))
    assert len(segments) == 1 and segments[0][2] is not None


def test_with_silences():
    segments = [(0.0, 30.0, 100.0), (30.0, 60.0, 120.0)]
    assert M._with_silences(segments, [(25.0, 35.0)]) == \
        [(0.0, 25.0, 100.0), (25.0, 35.0, None), (35.0, 60.0, 120.0)]
    #a leftover shorter than a silence goes to its neighbor
    assert M._with_silences([(0.0, 30.0, 100.0)], [(1.0, 10.0)]) == [(0.0, 10.0, None), (10.0, 30.0, 100.0)]


def test_bridge_short_runs():
    labels = [True, False, True, True, False, False, False, True]
    M._bridge_short_runs(labels, False, 2)
    assert labels == [True, True, True, True, False, False, False, True]


def test_unwrap():
    assert list(M._unwrap(np.array([0.9, 0.05, 0.1, 0.95]))) == pytest.approx([0.9, 1.05, 1.1, 0.95])


@pytest.mark.parametrize("bpm, previous, rpm", [
    (100, None, 100), (60, None, 120), (160, None, 80), (200, None, 100), (45, None, 90),
    (30, None, 120), (120, None, 120), (60, 50, 60), (110, 60, 55), (55, 100, 110), (250, None, 62.5),
])
def test_fold_rpm(bpm, previous, rpm):
    assert M.fold_rpm(bpm, previous) == pytest.approx(rpm)


def test_fold_rpm_narrow_range():
    assert M.fold_rpm(100, low=90, high=95) == 95


def test_pulses():
    beats = np.arange(0, 10, 0.5)   #120 BPM
    assert M._pulses(beats, 0, 10, 120) == pytest.approx(list(beats))
    assert M._pulses(beats, 0, 10, 60) == pytest.approx(list(beats[::2]))
    doubled = M._pulses(beats, 0, 2, 240)
    assert doubled == pytest.approx([0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5])
    assert M._pulses(beats, 0, 0.4, 120) == [] and M._pulses(beats, 0, 10, 0) == []


###The ride
def test_build_workout(songs):
    paths, analyses = songs
    order = [(paths["100"], None), (paths["75"], "Slow One"), ("missing.wav", None), (paths["gap"], None)]
    byPath = {paths[name]: analyses[name] for name in analyses}
    byPath["missing.wav"] = {"error": "gone"}
    blocks, tracks, songStarts, pulses = M.build_workout(order, byPath)
    assert [t[1] for t in tracks] == ["a_100", "Slow One", "c_gap"]
    assert [t[2] for t in tracks] == pytest.approx([0, 40, 80], abs=0.01)
    #the blocks add up to the rounded playlist length, each a whole number of seconds
    assert sum(d for d, _ in blocks) == 120 and all(isinstance(d, int) and d > 0 for d, _ in blocks)
    assert [rpm for _, rpm in blocks] == [100, 75, 100, 0, 100]
    assert songStarts == {0: 0, 1: 1, 2: 2}
    assert pulses == sorted(pulses) and pulses[-1] < 120
    assert M.song_header(tracks[1]) == "40s : Slow One"


def test_build_workout_merges_same_cadence_within_a_song():
    a = analysis([0.2]*15 + [0.21]*15)
    blocks, tracks, songStarts, pulses = M.build_workout([("s.wav", "S")], {"s.wav": a})
    assert len(blocks) == 1


###Cache and selection
def test_cache_round_trip(songs, tmp_path):
    paths, analyses = songs
    cache = M.load_cache()
    assert cache == {"version": M.cache_version(), "songs": {}}
    assert M.cached(cache, paths["100"]) is None
    cache["songs"][M._cache_key(paths["100"])] = analyses["100"]
    M.save_cache(cache)
    loaded = M.load_cache()
    assert M.cached(loaded, paths["100"]) == json.loads(json.dumps(analyses["100"]))
    assert loaded["crcs"] and loaded["crcs"][0][3] == M._crc(paths["100"], os.stat(paths["100"]))
    assert M.cached(loaded, "nowhere.wav") is None


def test_cache_from_another_version_is_dropped():
    with open(M.CACHE_FILE, "w") as f:
        json.dump({"version": {"analysis": -1}, "songs": {"x": {}}}, f)
    assert M.load_cache()["songs"] == {}
    with open(M.CACHE_FILE, "w") as f:
        f.write("{broken")
    assert M.load_cache()["songs"] == {}


def test_cache_key_follows_the_contents(tmp_path):
    first = tmp_path / "one" / "song.wav"
    second = tmp_path / "two" / "song.wav"
    for path, data in ((first, b"abc"), (second, b"abc")):
        path.parent.mkdir()
        path.write_bytes(data)
    #moved to another folder: the same song
    assert M._cache_key(str(first)) == M._cache_key(str(second))
    second.write_bytes(b"abd")
    #(a file's CRC is remembered by its size and modified time: an edit within the same clock tick would look
    #unchanged)
    stat = os.stat(second)
    os.utime(second, ns=(stat.st_atime_ns, stat.st_mtime_ns + 10**9))
    assert M._cache_key(str(first)) != M._cache_key(str(second))


def test_analyze_songs_uses_the_cache(songs):
    paths, analyses = songs
    cache = M.load_cache()
    cache["songs"][M._cache_key(paths["100"])] = {"duration": 1.0, "classes": [], "confidences": [], "beats": []}
    M.save_cache(cache)
    progress = []
    results = M.analyze_songs([paths["100"], paths["100"]], progress=lambda d, t: progress.append((d, t)))
    assert results[paths["100"]]["duration"] == 1.0 and progress == [(1, 1)]


def test_analyze_songs_in_a_worker_process(songs):
    paths, analyses = songs
    progress = []
    results = M.analyze_songs([paths["75"]], progress=lambda d, t: progress.append((d, t)), workers=1)
    assert results[paths["75"]]["classes"] == analyses["75"]["classes"]
    assert progress == [(0, 1), (1, 1)]
    #saved for next time
    assert M.cached(M.load_cache(), paths["75"]) is not None


def test_selection_round_trip():
    assert M.load_selection() == []
    M.save_selection([["a.mp3", "A", True], ["bad"], ["b.mp3", "B", False]])
    assert M.load_selection() == [["a.mp3", "A", True], ["b.mp3", "B", False]]
