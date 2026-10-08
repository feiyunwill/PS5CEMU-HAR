#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make the launcher's sounds: its music and its menu sounds, synthesised here.

    render-sounds.py OUTPUT_DIR

writes OUTPUT_DIR/music-setup.wav, a loop of PS5CEMU-HAR's own music in the spirit of a Nintendo
console's setup screen (light, swinging jazz), written for the launcher and taken from no game; and
move.wav, select.wav, back.wav, denied.wav and launch.wav, the menu's sounds. The music is IMA ADPCM
in stereo at 48 kHz (a quarter of 16-bit PCM's size), and the menu sounds 16-bit PCM in mono at 48
kHz: what port/frontend/sound.cpp reads. The piece ends where it starts, its last notes' ring
carried over its beginning, so it loops without a seam.

The files are in the repository (port/frontend/ui/sounds), so a build needs nothing from here; run
this again only to change them. It needs NumPy, and uses SciPy's filters when it is there (without
it, the same filters' responses are applied in the frequency domain). Each run makes the same files
(the playing's small unevenness is seeded).
"""

import os
import struct
import sys

import numpy as np

try:
    from scipy import signal
except ImportError:
    signal = None

SR = 48000


def hz(note):
    return 440.0 * 2 ** ((note - 69) / 12)


def seconds(length):
    return np.arange(int(round(length * SR))) / SR


def rise(t, length):
    return np.clip(t / length, 0, 1)


def fade_after(t, end, tau):
    """1 until end, then an exponential fade (a note let go of)."""
    return np.where(t < end, 1.0, np.exp(-np.maximum(t - end, 0) / tau))


_FILTERS = {}


def bandpass(x, low=None, high=None, order=2):
    if signal is None:
        return _fft_bandpass(x, low, high, order)
    key = (low, high, order)
    if key not in _FILTERS:
        if low and high:
            _FILTERS[key] = signal.butter(order, (low, high), "bandpass", fs=SR, output="sos")
        elif low:
            _FILTERS[key] = signal.butter(order, low, "highpass", fs=SR, output="sos")
        else:
            _FILTERS[key] = signal.butter(order, high, "lowpass", fs=SR, output="sos")
    return signal.sosfilt(_FILTERS[key], x)


def _fft_size(n):
    return 1 << max(1, int(n - 1).bit_length())


def _fft_bandpass(x, low, high, order):
    """a Butterworth filter's magnitude response, applied in the frequency domain (no SciPy)"""
    x = np.asarray(x, dtype=float)
    shape = x.shape
    flat = x.reshape(-1, shape[-1])
    size = _fft_size(shape[-1])
    f = np.fft.rfftfreq(size, 1 / SR)
    gain = np.ones_like(f)
    if high:
        gain /= np.sqrt(1 + (f / high) ** (2 * order))
    if low:
        gain /= np.sqrt(1 + (low / np.maximum(f, 1e-6)) ** (2 * order))
    out = np.fft.irfft(np.fft.rfft(flat, size, axis=-1) * gain, size, axis=-1)[:, :shape[-1]]
    return out.reshape(shape)


def convolve(x, response):
    """x convolved with response, as long as x"""
    if signal is not None:
        return signal.oaconvolve(x, response)[:len(x)]
    size = _fft_size(len(x) + len(response) - 1)
    return np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(response, size), size)[:len(x)]


class Band:
    """The instruments, and the drums, each note a mono sound; rng: the band's own randomness."""

    def __init__(self, seed):
        self.rng = np.random.default_rng(seed)

    def noise(self, n):
        return self.rng.standard_normal(n)

    # a Rhodes-like electric piano: two-operator FM whose brightness falls as the note rings, and the
    # tine's short ping
    def epiano(self, f, length, velocity):
        t = seconds(length + 0.6)
        index = velocity * 1.6 * np.exp(-t / 0.22) + 0.12
        tone = np.sin(2 * np.pi * f * t + index * np.sin(2 * np.pi * f * t))
        tone += 0.22 * velocity * np.sin(2 * np.pi * f * 7.0 * t) * np.exp(-t / 0.012)
        tau = float(np.clip(1.3 * (262 / f) ** 0.5, 0.5, 2.5))
        return tone * rise(t, 0.003) * np.exp(-t / tau) * fade_after(t, length, 0.09) * velocity

    # a vibraphone: the bar's partials and the mallet's tap (the tremolo is the mix's)
    def vibes(self, f, length, velocity):
        t = seconds(length + 1.6)
        tone = np.zeros_like(t)
        for ratio, level, tau in ((1.0, 1.0, 1.5 * (523 / f) ** 0.3), (3.99, 0.3, 0.2), (9.85, 0.06, 0.05)):
            if f * ratio < SR * 0.45:
                tone += level * np.exp(-t / tau) * np.sin(2 * np.pi * f * ratio * t)
        tone += 0.15 * bandpass(self.noise(len(t)), high=3000) * np.exp(-t / 0.003)
        return tone * rise(t, 0.002) * fade_after(t, length + 0.25, 0.25) * velocity

    # a soft flute, breath and all, its vibrato coming in as the note holds
    def flute(self, f, length, velocity):
        t = seconds(length + 0.35)
        depth = 0.0045 * np.clip((t - 0.18) / 0.35, 0, 1)
        phase = 2 * np.pi * np.cumsum(f * (1 + depth * np.sin(2 * np.pi * 5.1 * t))) / SR
        tone = np.sin(phase) + 0.1 * np.sin(2 * phase) + 0.03 * np.sin(3 * phase)
        tone += 0.05 * bandpass(self.noise(len(t)), 1800, 7000) * (0.4 + np.exp(-t / 0.05))
        return tone * (1 - np.exp(-t / 0.035)) * fade_after(t, length, 0.08) * velocity

    # a kalimba: its tine and the tine's high overtone
    def kalimba(self, f, velocity):
        t = seconds(1.3)
        tone = np.sin(2 * np.pi * f * t) * np.exp(-t / 0.55)
        if f * 5.9 < SR * 0.45:
            tone += 0.2 * np.sin(2 * np.pi * f * 5.9 * t) * np.exp(-t / 0.035)
        return tone * rise(t, 0.002) * velocity

    # a warm pad under the chords: two detuned voices of a soft sawtooth
    def pad(self, f, length, velocity):
        t = seconds(length + 1.2)
        tone = np.zeros_like(t)
        for detune in (-0.0035, 0.0035):
            for n in range(1, 7):
                if f * n > 6000:
                    break
                tone += np.sin(2 * np.pi * f * n * (1 + detune) * t + self.rng.uniform(0, 2 * np.pi)) / n ** 1.4
        return tone * (1 - np.exp(-t / 0.5)) * fade_after(t, length, 0.6) * velocity * 0.5

    # a round, plucked bass
    def bass(self, f, length, velocity):
        t = seconds(length + 0.12)
        phase = 2 * np.pi * f * t
        tone = np.sin(phase) + 0.3 * np.sin(2 * phase) * np.exp(-t / 0.25) + 0.1 * np.sin(3 * phase) * np.exp(-t / 0.12)
        return tone * rise(t, 0.005) * (0.35 + 0.65 * np.exp(-t / 0.35)) * fade_after(t, length * 0.92, 0.05) * velocity

    def shaker(self, velocity):
        t = seconds(0.09)
        return bandpass(self.noise(len(t)), low=5500) * rise(t, 0.012) ** 2 * np.exp(-t / 0.025) * velocity

    def rim(self, velocity):
        t = seconds(0.06)
        tone = 0.6 * bandpass(self.noise(len(t)), 1200, 5000) * np.exp(-t / 0.006)
        tone += 0.6 * np.sin(2 * np.pi * 1750 * t) * np.exp(-t / 0.01) + 0.3 * np.sin(2 * np.pi * 800 * t) * np.exp(-t / 0.015)
        return tone * velocity

    def kick(self, velocity):
        t = seconds(0.4)
        phase = 2 * np.pi * np.cumsum(46 + 70 * np.exp(-t / 0.03)) / SR
        return np.sin(phase) * rise(t, 0.002) * np.exp(-t / 0.16) * velocity

    def hat(self, velocity):
        t = seconds(0.07)
        return bandpass(self.noise(len(t)), low=7000) * np.exp(-t / 0.012) * velocity

    def brush(self, velocity):
        t = seconds(0.3)
        return bandpass(self.noise(len(t)), 1500, 7000) * rise(t, 0.04) * np.exp(-t / 0.09) * velocity


    # a ride cymbal: its bell's inharmonic ring over a wash of noise
    def ride(self, velocity, length=0.7):
        t = seconds(length)
        wash = bandpass(self.noise(len(t)), low=5000) * np.exp(-t / (length * 0.45))
        ping = sum(level * np.sin(2 * np.pi * f * t) for f, level in ((3150, 0.25), (4720, 0.18), (6380, 0.12)))
        return (0.7 * wash + ping * np.exp(-t / 0.18)) * rise(t, 0.0015) * velocity

    # a snare played softly: its head's tone and the wires
    def snare(self, velocity):
        t = seconds(0.18)
        wires = bandpass(self.noise(len(t)), 1800, 7000) * np.exp(-t / 0.045)
        head = np.sin(2 * np.pi * 190 * t) * np.exp(-t / 0.03)
        return (0.8 * wires + 0.5 * head) * rise(t, 0.001) * velocity


class Piece:
    """A piece being played: a mono bus for each part, the notes added where they fall."""

    def __init__(self, bpm, bars, seed, tail=3.0):
        self.beat = 60.0 / bpm
        self.length = int(round(bars * 4 * self.beat * SR))  # the loop
        self.total = self.length + int(tail * SR)  # and the last notes' ring
        self.buses = {}
        self.band = Band(seed)
        self.rng = np.random.default_rng(seed + 1)

    def add(self, bus, beat, sound, steady=False):
        """sound at a beat; a little early or late (steady: not), as played"""
        jitter = 0.0 if steady else self.rng.normal(0, 0.004)
        start = max(0, int(round((beat * self.beat + jitter) * SR)))
        end = min(self.total, start + len(sound))
        if end > start:
            self.buses.setdefault(bus, np.zeros(self.total))[start:end] += sound[:end - start]

    def velocity(self, value):
        return value * self.rng.uniform(0.9, 1.05)

    def mix(self, parts, reverb_time, wet):
        """the buses panned (with an optional auto-pan: depth, rate) and sent to a reverb, in stereo;
        then the ring past the loop's end laid over its start"""
        t = np.arange(self.total) / SR
        dry = np.zeros((2, self.total))
        send = np.zeros((2, self.total))
        for name, (gain, pan, reverb, wobble) in parts.items():
            if name not in self.buses:
                continue
            x = self.buses[name] * gain
            position = pan + (wobble[0] * np.sin(2 * np.pi * wobble[1] * t) if wobble else 0)
            angle = (np.clip(position, -1, 1) + 1) * np.pi / 4
            stereo = np.stack((x * np.cos(angle), x * np.sin(angle)))
            dry += stereo
            send += stereo * reverb
        out = dry + wet * reverberate(send, reverb_time, self.band)
        tail = self.total - self.length
        out[:, :tail] += out[:, self.length:]
        return out[:, :self.length]


def reverberate(x, reverb_time, band):
    """a hall: decaying noise, darker as it decays, for each side, after a short pre-delay"""
    t = seconds(reverb_time * 1.3)
    out = np.zeros_like(x)
    for channel in range(2):
        response = band.noise(len(t)) * np.exp(-6.91 * t / reverb_time)
        response = bandpass(response, high=5500)
        response[:int(0.015 * SR)] = 0
        response /= np.sqrt(np.sum(response ** 2))
        out[channel] = convolve(x[channel], response)
    return out


def master(stereo, loudness_db):
    """to a steady loudness (RMS, dBFS), its highest peaks rounded off below full scale. The filter
    starts a second before the start, on the loop's end, so it loops without a seam."""
    stereo = bandpass(np.concatenate((stereo[:, -SR:], stereo), axis=1), high=14000)[:, SR:]
    rms = np.sqrt(np.mean(stereo ** 2))
    stereo = stereo * (10 ** (loudness_db / 20) / rms)
    return np.tanh(stereo * 1.1) / 1.1


# -- the music ------------------------------------------------------------------------------------

SWING = 0.62  # where a swung off-beat eighth falls in its beat (straight would be 0.5)


def swung(beat):
    """a position in beats, its off-beat eighths swung"""
    whole = float(np.floor(beat))
    return whole + SWING if abs(beat - whole - 0.5) < 1e-6 else beat


def halves_of(chords, bar):
    chord = chords[bar % len(chords)]
    return chord if isinstance(chord, list) else [chord, chord]


def bass_near(pitch, near, low=36, high=52):
    """pitch's note in the bass's range nearest to near"""
    options = [n for n in range(low, high + 1) if n % 12 == pitch % 12]
    return min(options, key=lambda n: abs(n - near))


def walking_bass(piece, chords, bar, previous):
    """a bar of walking bass in quarters: the root, chord tones, and a half step into the next bar's
    root. Returns the last note, so the next bar walks on from it."""
    band = piece.band
    halves = halves_of(chords, bar)
    target = halves_of(chords, bar + 1)[0][0]
    notes = []
    near = previous
    for beat in range(4):
        root, voicing = halves[0] if beat < 2 else halves[1]
        if beat == 0 or (beat == 2 and halves[0] is not halves[1]):
            note = bass_near(root, near)
        elif beat == 3:
            goal = bass_near(target, near)
            note = goal + (1 if goal + 1 <= 52 and (piece.rng.random() < 0.5 or goal - 1 < 36) else -1)
        else:
            tones = [bass_near(n, near) for n in voicing if n % 12 != root % 12] + [bass_near(root + 7, near)]
            tones = [n for n in tones if n != near] or tones
            note = min(tones, key=lambda n: abs(abs(n - near) - 3) + piece.rng.uniform(0, 1.5))
        notes.append(note)
        near = note
    for beat, note in enumerate(notes):
        accent = 0.85 if beat in (0, 2) else 0.7
        piece.add("bass", bar * 4 + beat, band.bass(hz(note), 0.93 * piece.beat, piece.velocity(accent)))
    return notes[-1]


def comp(piece, chords, bar, pattern, level, bus="piano"):
    """the electric piano's chords on the pattern's swung beats; a hit on the last eighth takes the
    next bar's chord early, as a swinging pianist anticipates it"""
    band = piece.band
    halves = halves_of(chords, bar)
    following = halves_of(chords, bar + 1)[0]
    for beat, length in pattern:
        chord = following if beat >= 3.5 else (halves[0] if beat < 2 else halves[1])
        velocity = piece.velocity(level)
        for i, note in enumerate(chord[1]):
            piece.add(bus, bar * 4 + swung(beat) + i * 0.012, band.epiano(hz(note), length * piece.beat, velocity * (0.85 + 0.05 * i)))


def solo(piece, chords, bar, previous):
    """a bar of the vibraphone's solo over the bridge: chord tones on the beats, steps of the scale
    between them, the line turning at its edges. Returns the last note."""
    rhythms = [
        [(0.0, 0.5), (0.5, 0.5), (1.0, 1.0), (2.0, 0.5), (2.5, 0.5), (3.0, 1.0)],
        [(0.5, 0.5), (1.0, 0.5), (1.5, 0.5), (2.0, 1.5), (3.5, 0.5)],
        [(0.0, 1.5), (1.5, 0.5), (2.0, 0.5), (2.5, 0.5), (3.0, 0.5), (3.5, 0.5)],
        [(0.0, 2.0), (2.5, 0.5), (3.0, 1.0)],
    ]
    scale = {2, 4, 6, 7, 9, 11, 1}  # D major
    halves = halves_of(chords, bar)
    note = previous
    direction = 1 if previous < 81 else -1
    for beat, length in rhythms[bar % len(rhythms)]:
        voicing = (halves[0] if beat < 2 else halves[1])[1]
        if beat == int(beat):
            tones = [n for n in range(72, 90) if n % 12 in {v % 12 for v in voicing} and n != note]
            note = min(tones, key=lambda n: abs(n - (note + 3 * direction)))
        else:
            step = note + direction
            while step % 12 not in scale:
                step += direction
            note = step
        if note >= 87 or note <= 73 or piece.rng.random() < 0.18:
            direction = -direction
        piece.add("vibes", bar * 4 + swung(beat), piece.band.vibes(hz(note), length * piece.beat * 0.9, piece.velocity(0.7)))
    return note


def setup():
    """'Setup': a light, swinging piece in D, 100 beats a minute, 40 bars. The tune (16 bars) on the
    vibraphone over the kalimba's arpeggios and brushes; again on the flute with the vibraphone an
    octave under it, the bass walking and the ride swinging; then an 8-bar bridge, the vibraphone
    improvising, and a fill back to the top."""
    DMAJ9, BM9, GMAJ9 = (38, (54, 57, 61, 64)), (47, (62, 66, 69, 73)), (43, (54, 57, 59, 62))
    A13SUS, A7, FSM9 = (45, (55, 59, 62, 66)), (45, (55, 61, 64)), (42, (57, 61, 64, 68))
    BM9B, EM9, A7SUS = (47, (57, 61, 62, 66)), (40, (55, 59, 62, 66)), (45, (55, 59, 62, 64))
    FSM7, D_F, GM6, D_A = (42, (57, 61, 64)), (42, (57, 62, 64, 66)), (43, (58, 62, 64)), (45, (54, 57, 61, 64))
    B7B9, A13, E9 = (47, (57, 60, 63, 66)), (45, (55, 61, 66, 71)), (40, (56, 62, 66, 71))
    progression = [DMAJ9, BM9, GMAJ9, [A13SUS, A7], FSM9, BM9B, EM9, [A7SUS, A7],
                   GMAJ9, FSM7, EM9, D_F, GMAJ9, GM6, D_A, [A13SUS, A7]]
    bridge = [GMAJ9, [FSM7, B7B9], EM9, A13, D_F, [BM9, E9], EM9, [A7SUS, A7]]
    chords = progression + progression + bridge
    tune = len(progression)
    melody = [
        [(0.0, 1.5, 78), (1.5, 0.5, 76), (2.0, 1.0, 81), (3.0, 1.0, 85)],
        [(0.0, 2.0, 86), (2.0, 1.0, 85), (3.0, 1.0, 81)],
        [(0.0, 3.0, 83), (3.0, 0.5, 81), (3.5, 0.5, 78)],
        [(0.0, 2.0, 76), (2.0, 0.5, 78), (2.5, 0.5, 79), (3.0, 1.0, 81)],
        [(0.0, 1.5, 80), (1.5, 0.5, 81), (2.0, 1.0, 85), (3.0, 1.0, 83)],
        [(0.0, 2.0, 85), (2.0, 1.0, 86), (3.0, 1.0, 78)],
        [(0.0, 1.5, 79), (1.5, 0.5, 78), (2.0, 1.0, 74), (3.0, 1.0, 71)],
        [(0.0, 2.0, 76), (2.0, 2.0, 73)],
        [(0.0, 1.0, 74), (1.0, 1.0, 78), (2.0, 1.0, 81), (3.0, 1.0, 83)],
        [(0.0, 2.0, 85), (2.0, 1.0, 81), (3.0, 1.0, 76)],
        [(0.0, 1.5, 78), (1.5, 0.5, 79), (2.0, 2.0, 83)],
        [(0.0, 2.0, 81), (2.0, 1.0, 78), (3.0, 1.0, 76)],
        [(0.0, 1.0, 74), (1.0, 1.0, 76), (2.0, 1.0, 78), (3.0, 1.0, 81)],
        [(0.0, 2.0, 82), (2.0, 1.0, 81), (3.0, 1.0, 79)],
        [(0.0, 3.0, 78)],
        [(2.0, 1.0, 76), (3.0, 1.0, 73)],
    ]
    calm = [(0.0, 1.4), (2.5, 1.3)]
    charleston = ([(0.0, 0.6), (1.5, 0.45), (3.0, 0.8)], [(1.0, 0.45), (2.5, 0.9), (3.5, 0.5)])
    piece = Piece(100, len(chords), seed=100)
    band = piece.band
    bass = 38
    line = 81
    for bar in range(len(chords)):
        halves = halves_of(chords, bar)
        start = bar * 4
        section = 0 if bar < tune else 1 if bar < 2 * tune else 2
        # the piano: calm under the tune's first statement, then swinging
        if section == 0:
            comp(piece, chords, bar, calm, 0.42)
        else:
            comp(piece, chords, bar, charleston[bar % 2], 0.5 if section == 1 else 0.55)
        # the pad, under the tune's first statement and the bridge
        if section != 1:
            for half, (beat, length) in enumerate(((0, 2), (2, 2)) if halves[0] is not halves[1] else ((0, 4),)):
                for note in halves[half][1]:
                    piece.add("pad", start + beat, band.pad(hz(note + 12), length * piece.beat, 0.5), steady=True)
        # the bass: a two-feel first, walking after
        if section == 0:
            following = halves_of(chords, bar + 1)[0]
            first, second = halves[0][0], halves[1][0]
            fifth = first + 7 if first + 7 <= 52 else first - 5
            for beat, length, note in ((0, 1.9, first), (2, 1.4, second if halves[0] is not halves[1] else fifth), (3.5, 0.45, following[0])):
                piece.add("bass", start + swung(beat), band.bass(hz(note), length * piece.beat, piece.velocity(0.75)))
            bass = following[0]
        else:
            bass = walking_bass(piece, chords, bar, bass)
        # the kalimba's arpeggios, fading back as the band fills in
        level = (0.45, 0.22, 0.28)[section]
        for eighth in range(8):
            voicing = sorted(n + 12 for n in halves[eighth // 4][1])
            tones = voicing + [voicing[0] + 12, voicing[1] + 12]
            note = tones[(0, 2, 1, 3, 2, 4, 3, 1)[eighth]]
            velocity = piece.velocity(level * (1.0 if eighth % 2 == 0 else 0.75))
            piece.add("bells-left" if eighth % 2 == 0 else "bells-right", start + swung(eighth / 2), band.kalimba(hz(note), velocity))
        # the drums
        if section == 0:
            # brushes: the hat's swung pattern, the brush on 2 and 4, a soft kick
            for beat, velocity in ((0, 0.5), (1, 0.45), (1.5, 0.3), (2, 0.5), (3, 0.45), (3.5, 0.3)):
                piece.add("hat", start + swung(beat), band.hat(piece.velocity(velocity)))
            for beat in (1, 3):
                piece.add("brush", start + beat - 0.04, band.brush(piece.velocity(0.6)))
            for beat, velocity in ((0, 0.65), (2.5, 0.35)):
                piece.add("kick", start + swung(beat), band.kick(piece.velocity(velocity)), steady=True)
        else:
            # the ride's "ding, ding-a ding", the hat's foot on 2 and 4, the kick feathered, ghost notes
            for beat, velocity in ((0, 0.7), (1, 0.75), (1.5, 0.45), (2, 0.7), (3, 0.75), (3.5, 0.45)):
                piece.add("ride", start + swung(beat), band.ride(piece.velocity(velocity)))
            for beat in (1, 3):
                piece.add("hat", start + beat, band.hat(piece.velocity(0.4)))
            for beat in range(4):
                piece.add("kick", start + beat, band.kick(piece.velocity(0.22)), steady=True)
            for beat in (1.5, 3.5):
                if piece.rng.random() < 0.4:
                    piece.add("snare", start + swung(beat), band.snare(piece.velocity(0.25)))
            if bar in (tune, 2 * tune):
                piece.add("ride", start, band.ride(piece.velocity(1.0), length=2.2))  # a crash into the section
        # the fill back to the top: a snare triplet and the kick under its last note
        if bar == len(chords) - 1:
            for i, beat in enumerate((3.0, 3 + 1 / 3, 3 + 2 / 3)):
                piece.add("snare", start + beat, band.snare(piece.velocity(0.45 + 0.15 * i)), steady=True)
            piece.add("kick", start + 3 + 2 / 3, band.kick(piece.velocity(0.6)), steady=True)
        # the tune: the vibraphone, then the flute with the vibraphone an octave under it; the solo
        if section == 0:
            for beat, length, note in melody[bar]:
                piece.add("vibes", start + swung(beat), band.vibes(hz(note), length * piece.beat * 0.95, piece.velocity(0.7)))
        elif section == 1:
            for beat, length, note in melody[bar - tune]:
                piece.add("flute", start + swung(beat), band.flute(hz(note), length * piece.beat * 0.95, piece.velocity(0.75)))
                piece.add("vibes-low", start + swung(beat), band.vibes(hz(note - 12), length * piece.beat * 0.95, piece.velocity(0.5)))
        else:
            line = solo(piece, chords, bar, line)
    return piece.mix({
        # gain, pan, reverb send, auto-pan (depth, rate)
        "piano": (0.12, 0.0, 0.3, (0.25, 1.6)),
        "pad": (0.03, 0.0, 0.5, None),
        "bass": (0.4, 0.0, 0.04, None),
        "bells-left": (0.1, -0.5, 0.45, None),
        "bells-right": (0.1, 0.5, 0.45, None),
        "vibes": (0.22, 0.2, 0.35, (0.12, 5.5)),
        "vibes-low": (0.14, -0.2, 0.35, (0.1, 5.5)),
        "flute": (0.2, 0.1, 0.4, None),
        "hat": (0.035, 0.35, 0.1, None),
        "ride": (0.05, 0.4, 0.15, None),
        "snare": (0.06, -0.2, 0.2, None),
        "brush": (0.04, -0.25, 0.2, None),
        "kick": (0.2, 0.0, 0.0, None),
    }, reverb_time=2.0, wet=0.55)


# -- the menu's sounds ----------------------------------------------------------------------------

def chime(f, length, tau, harmonics=(1.0, 0.3, 0.1)):
    t = seconds(length)
    tone = np.zeros_like(t)
    for n, level in enumerate(harmonics, 1):
        tone += level * np.sin(2 * np.pi * f * n * t) * np.exp(-t * n / tau)
    return tone * rise(t, 0.002)


def place(sounds, length):
    """sounds at their times (seconds), mixed into one"""
    out = np.zeros(int(length * SR))
    for at, sound in sounds:
        start = int(at * SR)
        end = min(len(out), start + len(sound))
        out[start:end] += sound[:end - start]
    return out


def room(x, band):
    """a small room's ring, so the sounds sit softly"""
    t = seconds(0.4)
    response = band.noise(len(t)) * np.exp(-6.91 * t / 0.35)
    response = bandpass(response, high=6000)
    response /= np.sqrt(np.sum(response ** 2))
    return x + 0.18 * convolve(x, response)


def effects():
    band = Band(7)
    sounds = {}
    # moving: a soft rising blip
    t = seconds(0.09)
    phase = 2 * np.pi * np.cumsum(1100 * (1 + 0.35 * (1 - np.exp(-t / 0.012)))) / SR
    blip = (np.sin(phase) + 0.25 * np.sin(2 * phase) * np.exp(-t / 0.01)) * rise(t, 0.0015) * np.exp(-t / 0.022)
    sounds["move"] = (blip, 0.16)
    # choosing: two bright notes up a fifth
    sounds["select"] = (place([(0, chime(hz(88), 0.5, 0.12)), (0.06, 0.9 * chime(hz(95), 0.5, 0.16))], 0.6), 0.26)
    # going back: two rounder notes down a fifth
    sounds["back"] = (place([(0, chime(hz(83), 0.35, 0.07, (1.0, 0.15))), (0.055, chime(hz(76), 0.35, 0.1, (1.0, 0.15)))], 0.45), 0.2)
    # nothing to choose there (yet): two low, muffled bumps
    def bump(f):
        t = seconds(0.09)
        tone = sum(np.sin(2 * np.pi * f * n * t) / n for n in (1, 3, 5))
        return bandpass(tone, high=1200) * rise(t, 0.003) * np.exp(-t / 0.035)
    sounds["denied"] = (place([(0, bump(196)), (0.1, bump(185))], 0.3), 0.24)
    # a game starting: a rising arpeggio and a sparkle
    arpeggio = [(i * 0.05, chime(hz(note), 1.0, 0.25)) for i, note in enumerate((84, 88, 91, 96))]
    t = seconds(0.8)
    sparkle = sum(np.sin(2 * np.pi * f * t) * np.exp(-t / 0.18) * 0.12 for f in (4186, 5274, 6272))
    sparkle *= rise(t, 0.03)
    sounds["launch"] = (place(arpeggio + [(0.2, sparkle)], 1.1), 0.28)
    out = {}
    for name, (sound, peak) in sounds.items():
        sound = room(sound, band)
        out[name] = sound * (peak / np.max(np.abs(sound)))
    return out


# -- files ----------------------------------------------------------------------------------------

IMA_STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
             73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
             494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
             2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
             10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]
BLOCK_ALIGN = 2048  # bytes a block, both channels


def to_pcm16(x):
    return np.clip(np.round(x * 32767), -32768, 32767).astype(np.int16)


def write_pcm(path, mono):
    data = to_pcm16(mono).tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SR, SR * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def write_adpcm(path, stereo):
    """Microsoft's IMA ADPCM WAV: blocks of a header for each channel (its first sample and step
    index), then 4-byte groups of eight 4-bit samples, a channel's at a time"""
    samples = to_pcm16(stereo).astype(np.int32)
    channels = samples.shape[0]
    per_block = (BLOCK_ALIGN - 4 * channels) * 8 // (4 * channels) + 1
    frames = samples.shape[1]
    blocks = -(-frames // per_block)
    padded = np.zeros((channels, blocks * per_block), dtype=np.int32)
    padded[:, :frames] = samples
    state = [[0, 0] for _ in range(channels)]  # predictor, step index
    out = bytearray()
    for block in range(blocks):
        chunk = padded[:, block * per_block:(block + 1) * per_block].tolist()
        for c in range(channels):
            state[c][0] = chunk[c][0]
            out += struct.pack("<hBB", chunk[c][0], state[c][1], 0)
        codes = [encode_ima(chunk[c][1:], state[c]) for c in range(channels)]
        for group in range((per_block - 1) // 8):
            for c in range(channels):
                nibbles = codes[c][group * 8:group * 8 + 8]
                out += bytes(nibbles[i] | (nibbles[i + 1] << 4) for i in range(0, 8, 2))
    with open(path, "wb") as f:
        fmt = struct.pack("<HHIIHHHH", 0x11, channels, SR, SR * BLOCK_ALIGN // per_block, BLOCK_ALIGN, 4, 2, per_block)
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 12 + 8 + len(out)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        f.write(b"fact" + struct.pack("<II", 4, frames))
        f.write(b"data" + struct.pack("<I", len(out)) + out)


def encode_ima(samples, state):
    predictor, index = state
    codes = []
    for sample in samples:
        step = IMA_STEPS[index]
        difference = sample - predictor
        code = 0
        if difference < 0:
            code = 8
            difference = -difference
        delta = step >> 3
        if difference >= step:
            code |= 4
            difference -= step
            delta += step
        if difference >= step >> 1:
            code |= 2
            difference -= step >> 1
            delta += step >> 1
        if difference >= step >> 2:
            code |= 1
            delta += step >> 2
        predictor = predictor - delta if code & 8 else predictor + delta
        predictor = -32768 if predictor < -32768 else 32767 if predictor > 32767 else predictor
        index += IMA_INDEX[code & 7]
        index = 0 if index < 0 else 88 if index > 88 else index
        codes.append(code)
    state[0], state[1] = predictor, index
    return codes


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    for name, sound in effects().items():
        write_pcm(os.path.join(folder, f"{name}.wav"), sound)
    stereo = master(setup(), loudness_db=-20)
    write_adpcm(os.path.join(folder, "music-setup.wav"), stereo)
    print(f"music-setup.wav: {stereo.shape[1] / SR:.1f} s, peak {20 * np.log10(np.max(np.abs(stereo))):.1f} dBFS")
    if os.environ.get("PREVIEW"):
        # the same, as 16-bit PCM, to listen to anywhere
        data = to_pcm16(stereo).T.tobytes()
        with open(os.environ["PREVIEW"], "wb") as f:
            f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
            f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, SR, SR * 4, 4, 16))
            f.write(b"data" + struct.pack("<I", len(data)) + data)


if __name__ == "__main__":
    main()
