"""Stereo audio buffers and sounddevice, soundcard, and silent outputs."""

from __future__ import annotations

from collections import deque
import math
import threading
import time
from typing import Callable, Optional

import numpy as np


class AudioError(RuntimeError):
    pass


class StereoRing:
    """Fixed capacity interleaved stereo float32 ring buffer."""

    def __init__(self, capacity_frames: int):
        self.capacity = int(capacity_frames)
        self._buf = np.zeros((self.capacity, 2), dtype=np.float32)
        self._unscaled = np.zeros_like(self._buf)
        self._paused_gain = None
        self._output_gain = None
        self._target_gain = 1.0
        self._gain_left = 0
        self._output_level = (0.0, 0.0)
        self._w = 0
        self._r = 0
        self._count = 0
        self._lock = threading.Lock()
        self.overflow_frames = 0
        self.underflow_frames = 0
        # Keep row markers and output times in bounded histories under the ring lock.
        self._written = 0
        self._marks = deque(maxlen=8192)
        self._playback = deque(maxlen=8192)
        self._positions = deque(maxlen=8192)
        self._cleared_at = 0
        self._paused = False
        self._attack_frames = 0
        self._attack_left = 0

    def write(self, data: np.ndarray, *, before=None, after=None,
              position_before=None, position_after=None, unscaled=None) -> int:
        n = len(data)
        if n <= 0:
            return 0
        with self._lock:
            if n > self.capacity:
                self.overflow_frames += n - self.capacity
                if unscaled is not None:
                    unscaled = unscaled[n - self.capacity:]
                data = data[n - self.capacity:]
                n = self.capacity
            free = self.capacity - self._count
            if n > free:  # drop the oldest audio rather than blocking the renderer
                self.overflow_frames += n - free
                drop = n - free
                self._r = (self._r + drop) % self.capacity
                self._count -= drop
            if before is not None and not self._marks:
                self._marks.append((self._written, before))
            source = data if unscaled is None else unscaled
            if self._paused_gain is not None:
                data = source * self._paused_gain
            first = min(n, self.capacity - self._w)
            self._unscaled[self._w : self._w + first] = source[:first]
            self._buf[self._w : self._w + first] = data[:first]
            rest = n - first
            if rest:
                self._buf[:rest] = data[first:]
                self._unscaled[:rest] = source[first:]
            self._w = (self._w + n) % self.capacity
            self._count += n
            self._written += n
            if before is not None and position_before is not None and position_after is not None:
                self._positions.append((self._written - n, self._written, before[0],
                                        position_before, position_after))
            if after is not None and (not self._marks or self._marks[-1][1] != after):
                # Attach each row change to the end of its rendered block.
                self._marks.append((self._written, after))
            return n

    def read_into(self, out: np.ndarray, frames: Optional[int] = None, *,
                  playback_time: Optional[float] = None, samplerate: float = 0.0) -> int:
        """Fill out (shape (n, 2)), zero-fills whatever is not available."""
        want = len(out) if frames is None else int(min(frames, len(out)))
        with self._lock:
            if self._paused:
                out[:want] = 0.0
                self._output_level = (0.0, 0.0)
                return 0
            have = min(want, self._count)
            if have and playback_time is not None and samplerate > 0:
                self._playback.append((playback_time, self._written - self._count,
                                       have, samplerate))
            if have:
                first = min(have, self.capacity - self._r)
                out[:first] = self._buf[self._r : self._r + first]
                rest = have - first
                if rest:
                    out[first : first + rest] = self._buf[:rest]
                self._r = (self._r + have) % self.capacity
                self._count -= have
            if have < want:
                out[have:want] = 0.0
                self.underflow_frames += want - have
            if self._output_gain is not None:
                # Advance through output silence too: late PCM must not revive an old gain.
                self._apply_output_gain(out, want)
            if have and self._attack_left:
                n = min(have, self._attack_left)
                start = self._attack_frames - self._attack_left
                ramp = np.arange(start, start + n, dtype=np.float32) / self._attack_frames
                out[:n] *= ramp[:, None]
                self._attack_left -= n
            if self._output_gain is not None and want:
                left = float(np.abs(out[:want, 0]).max())
                right = float(np.abs(out[:want, 1]).max())
                self._output_level = (max(left, self._output_level[0] * 0.82),
                                      max(right, self._output_level[1] * 0.82))
        return have

    def set_output_gain(self, gain: float, ramp_frames: int = 0) -> None:
        """Apply master gain on consumption, never by rewriting or discarding queued PCM."""
        gain = max(0.0, min(1.0, float(gain)))
        with self._lock:
            if self._output_gain is None or self._paused or ramp_frames <= 0:
                self._output_gain = self._target_gain = gain
                self._gain_left = 0
            elif gain != self._target_gain:
                self._target_gain = gain
                self._gain_left = int(ramp_frames)

    def _apply_output_gain(self, out: np.ndarray, have: int) -> None:
        # Called under the ring lock. Continue ramps across arbitrary callback sizes.
        n = min(have, self._gain_left)
        if n:
            ramp = (self._output_gain + (self._target_gain - self._output_gain)
                    * (np.arange(1, n + 1, dtype=np.float32) / self._gain_left))
            if n == self._gain_left:
                ramp[-1] = self._target_gain
            out[:n] *= ramp[:, None]
            self._gain_left -= n
            self._output_gain = float(ramp[-1]) if self._gain_left else self._target_gain
        if have > n and self._output_gain != 1.0:
            out[n:have] *= self._output_gain

    def output_level(self) -> tuple[float, float]:
        with self._lock:
            return self._output_level

    def clear(self) -> None:
        with self._lock:
            self._r = self._w
            self._count = 0
            # Clear old timing on seek or load, do not reuse frame indices.
            self._marks.clear()
            self._playback.clear()
            self._positions.clear()
            self._cleared_at = self._written
            self._paused_gain = None
            self._attack_left = 0
            self._output_level = (0.0, 0.0)
            if self._output_gain is not None:
                self._output_gain = self._target_gain
                self._gain_left = 0

    def _playback_frame(self, now):
        if self._paused or not self._playback:
            return self._written - self._count
        while len(self._playback) > 1 and self._playback[1][0] <= now:
            self._playback.popleft()
        start, frame, count, rate = self._playback[0]
        return frame + min(count, max(0.0, (now - start) * rate))

    def playback_mark(self, now: Optional[float] = None):
        """Return the output-timed row, hold it while paused or through silence."""
        now = time.monotonic() if now is None else now
        with self._lock:
            if not self._marks:
                return None
            if not self._playback and not self._paused:
                return self._marks[0][1]
            frame = self._playback_frame(now)
            while len(self._marks) > 1 and self._marks[1][0] <= frame:
                self._marks.popleft()
            return self._marks[0][1]

    def pause(self, *, rewind=False, now=None) -> None:
        """Hold PCM unchanged, reclaim submitted frames only when the device discards them."""
        now = time.monotonic() if now is None else now
        with self._lock:
            if self._paused:
                return
            frame = int(self._playback_frame(now)) if rewind else self._written - self._count
            frame = max(self._cleared_at, self._written - self.capacity, frame)
            self._count = min(self.capacity, max(0, self._written - frame))
            self._r = (self._w - self._count) % self.capacity
            self._paused = True
            self._output_level = (0.0, 0.0)
            if self._output_gain is not None:
                self._output_gain = self._target_gain
                self._gain_left = 0
            self._playback.clear()
            while len(self._marks) > 1 and self._marks[1][0] <= frame:
                self._marks.popleft()

    def set_paused_gain(self, gain: float) -> None:
        """Apply volume changes to held PCM, including unmuting previously silent audio."""
        with self._lock:
            if not self._paused:
                return
            if self._output_gain is not None:
                self._output_gain = self._target_gain = gain
                self._gain_left = 0
                return
            self._paused_gain = gain
            first = min(self._count, self.capacity - self._r)
            self._buf[self._r:self._r + first] = self._unscaled[self._r:self._r + first] * gain
            rest = self._count - first
            if rest:
                self._buf[:rest] = self._unscaled[:rest] * gain

    def resume(self, ramp_frames: int = 0) -> None:
        with self._lock:
            if self._paused:
                self._attack_frames = max(0, int(ramp_frames))
                self._attack_left = self._attack_frames
            self._paused = False
            self._paused_gain = None

    @property
    def paused(self) -> bool:
        with self._lock:
            return self._paused

    def paused_position(self):
        with self._lock:
            if not self._paused:
                return None
            frame = self._written - self._count
            for start, end, identity, before, after in reversed(self._positions):
                if start <= frame <= end:
                    fraction = (frame - start) / max(1, end - start)
                    return identity, before + (after - before) * fraction
        return None

    def fade_out(self, frames: int) -> None:
        """Fade the last buffered frames to silence."""
        n = int(min(frames, self._count))
        if n <= 0:
            return
        with self._lock:
            start = (self._r + self._count - n) % self.capacity
            ramp = np.linspace(1.0, 0.0, n, endpoint=False, dtype=np.float32)[:, None]
            first = min(n, self.capacity - start)
            self._buf[start : start + first] *= ramp[:first]
            rest = n - first
            if rest:
                self._buf[:rest] *= ramp[first:]

    def available(self) -> int:
        with self._lock:
            return 0 if self._paused else self._count

    def drained(self) -> bool:
        with self._lock:
            return self._count == 0 and self._playback_frame(time.monotonic()) >= self._written

    def pending(self) -> int:
        with self._lock:
            return self._count

    def space(self) -> int:
        with self._lock:
            return self.capacity - self._count


class OutputDevice:
    """Base class.  Concrete backends call self._pull(out_view) for audio."""

    name = "none"
    description = ""

    def __init__(
        self,
        ring: StereoRing,
        samplerate: int,
        blocksize: int = 1024,
        latency_ms: float = 20.0,
        device=None,
        speed: float = 1.0,
    ):
        self.ring = ring
        self.samplerate = int(samplerate)
        self.blocksize = int(blocksize)
        self.latency_ms = float(latency_ms)
        self.device = device
        self.speed = float(speed)
        self.xruns = 0
        self.last_status = ""
        self._running = False

    def start(self) -> None:  # pragma: no cover - overridden
        self._running = True

    def stop(self) -> None:  # pragma: no cover - overridden
        self._running = False

    def is_running(self) -> bool:
        return self._running

    def pause(self) -> None:
        self.ring.pause()

    def resume(self) -> None:
        # Ease into the held waveform over 2 ms, without inserting silence or skipping frames.
        self.ring.resume(ramp_frames=max(1, round(self.samplerate * 0.002)))

    def buffered_seconds(self) -> float:
        return self.ring.available() / float(self.samplerate)

    def _zero(self, n: int) -> np.ndarray:
        return np.zeros((n, 2), dtype=np.float32)

    @staticmethod
    def available_backends() -> list[str]:
        return ["sounddevice", "soundcard", "null"]


class NullOutput(OutputDevice):
    """Consume audio silently at the requested playback speed."""

    name = "null"
    description = "silent output (no audio device)"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._buf = self._zero(self.blocksize)

    def start(self) -> None:
        self._stop.clear()
        self._running = True
        self._thread = threading.Thread(target=self._loop, name="null-audio", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None

    def pause(self) -> None:
        self.ring.pause(rewind=True)

    def _loop(self) -> None:
        period = self.blocksize / float(self.samplerate) / max(self.speed, 1e-9)
        next_t = time.monotonic()
        while not self._stop.is_set():
            self.ring.read_into(self._buf, playback_time=time.monotonic(),
                                samplerate=self.samplerate * max(self.speed, 1e-9))
            if self.speed <= 50.0:  # keep real-time pacing for sane speeds
                next_t += period
                delay = next_t - time.monotonic()
                if delay > 0:
                    self._stop.wait(delay)
                elif delay < -period * 4:
                    next_t = time.monotonic()
            else:
                time.sleep(0)  # yield


class SoundDeviceOutput(OutputDevice):
    """PortAudio backend."""

    name = "sounddevice"
    description = "PortAudio"
    _cached_rate: Optional[float] = None

    @classmethod
    def default_samplerate(cls, device=None) -> Optional[int]:
        try:
            import sounddevice as sd

            info = sd.query_devices(device, kind="output")
            return int(round(float(info["default_samplerate"])))
        except Exception:
            return None

    def _playback_delay(self, time_info) -> float:
        """Use device timestamps to find the playback delay, fall back to stream or requested
        latency."""
        try:
            dac, current = float(time_info.outputBufferDacTime), float(time_info.currentTime)
            delay = dac - current
            if dac != 0 and math.isfinite(dac) and math.isfinite(current) and math.isfinite(delay):
                return max(0.0, delay)
        except (AttributeError, TypeError, ValueError):
            pass
        latency = getattr(getattr(self, "_stream", None), "latency", self.latency_ms / 1000.0)
        try:
            latency = float(latency)
            if math.isfinite(latency) and latency >= 0:
                return latency
        except (TypeError, ValueError):
            pass
        return max(0.0, self.latency_ms / 1000.0)

    def start(self) -> None:
        import sounddevice as sd

        self._sd = sd
        self._status_counts: dict[str, int] = {}

        def callback(outdata, frames, _time_info, status):
            playback_time = time.monotonic() + self._playback_delay(_time_info)
            if (status.input_underflow or status.input_overflow
                    or status.output_underflow or status.output_overflow):
                text = str(status)
                self._status_counts[text] = self._status_counts.get(text, 0) + 1
                self.xruns += 1
                self.last_status = text
            self.ring.read_into(outdata, playback_time=playback_time,
                                samplerate=self.samplerate)

        try:
            self._stream = sd.OutputStream(
                samplerate=self.samplerate,
                channels=2,
                dtype="float32",
                blocksize=0,
                device=self.device,
                latency=max(self.latency_ms, 20.0) / 1000.0,
                callback=callback,
                prime_output_buffers_using_stream_callback=True,
            )
            self._stream.start()
            self._running = True
            # PortAudio may have picked a different rate if we asked for a weird one
            self.samplerate = int(round(self._stream.samplerate))
        except Exception as exc:
            raise AudioError(f"sounddevice: {exc}") from exc

    def pause(self) -> None:
        if self.ring.paused:
            return
        self.ring.pause(rewind=True)
        stream = getattr(self, "_stream", None)
        if stream is not None:
            stream.abort()  # discard queued music without draining it
            # Keep the output clock running on silence so Resume needs no device restart.
            stream.start()

    def stop(self) -> None:
        self._running = False
        stream = getattr(self, "_stream", None)
        if stream is not None:
            try:
                stream.stop()
            except Exception:
                pass
            try:
                stream.close()
            except Exception:
                pass
            self._stream = None

    def is_running(self) -> bool:
        stream = getattr(self, "_stream", None)
        try:
            return bool(stream is not None and not stream.closed and stream.active)
        except Exception:
            return False


class SoundCardOutput(OutputDevice):
    """soundcard output. Its play() calls queue audio and wait for space as needed."""

    name = "soundcard"
    description = "PulseAudio / PipeWire / WASAPI"

    def start(self) -> None:
        import soundcard as sc

        self._sc = sc
        try:
            speaker = self.device
            if speaker is None:
                speaker = sc.default_speaker()
            self.device = speaker
            if speaker is None:
                raise AudioError("soundcard: no default speaker")
            # no data_type: soundcard takes nothing else than float32
            player = speaker.player(
                samplerate=self.samplerate,
                channels=2,
                blocksize=self.blocksize,
            )
            player.__enter__()      # opens the stream, kept open until stop()
            self._player = player
        except AudioError:
            raise
        except Exception as exc:
            raise AudioError(f"soundcard: {exc}") from exc

        self._block = np.zeros((int(self.blocksize), 2), dtype=np.float32)
        self._stop = threading.Event()
        self._running = True
        self._thread = threading.Thread(target=self._loop, name="soundcard-audio", daemon=True)
        self._thread.start()

    def _loop(self) -> None:
        try:
            while not self._stop.is_set():
                # soundcard timing is approximate, only Linux exposes a stream-latency estimate.
                delay = 0.0
                try:
                    latency = float(getattr(self._player, "latency", 0.0))
                    if math.isfinite(latency) and latency >= 0:
                        delay = latency
                except Exception:
                    pass  # metering must never interrupt playback
                self.ring.read_into(self._block, playback_time=time.monotonic() + delay,
                                    samplerate=self.samplerate)
                self._player.play(self._block)
        except Exception as exc:  # device unplugged / server restarted
            self.last_status = str(exc)
            self.xruns += 1
            self._running = False

    def stop(self) -> None:
        self._running = False
        stop = getattr(self, "_stop", None)
        if stop is not None:
            stop.set()
        thread = getattr(self, "_thread", None)
        if thread is not None:
            thread.join(timeout=1.0)
            self._thread = None
        player = getattr(self, "_player", None)
        if player is not None:
            try:
                player.__exit__(None, None, None)
            except Exception:
                pass
            self._player = None

    def is_running(self) -> bool:
        return bool(self._running and getattr(self, "_thread", None) is not None
                    and self._thread.is_alive())


def _instantiate(name: str, ring, samplerate, blocksize, latency_ms, device, speed):
    cls = {"sounddevice": SoundDeviceOutput, "soundcard": SoundCardOutput, "null": NullOutput}[name]
    return cls(
        ring=ring,
        samplerate=samplerate,
        blocksize=blocksize,
        latency_ms=latency_ms,
        device=device,
        speed=speed,
    )


def create_output(
    prefer: str = "auto",
    ring: Optional[StereoRing] = None,
    samplerate: Optional[int] = None,
    blocksize: int = 1024,
    latency_ms: float = 20.0,
    device=None,
    speed: float = 1.0,
    log: Optional[Callable[[str], None]] = None,
) -> tuple[OutputDevice, StereoRing]:
    """Return (output, ring), trying audio backends in preference order."""
    log = log or (lambda _m: None)
    order = ["sounddevice", "soundcard", "null"] if prefer in ("auto", "", None) else [prefer, "null"]
    errors = []
    for name in order:
        rate = samplerate
        if rate is None:
            if name == "sounddevice":
                rate = SoundDeviceOutput.default_samplerate(device) or 48000
            else:
                rate = 48000
        out = None
        try:
            r = ring
            if r is None:
                cap = int(max(rate * 5.0, blocksize * 8))  # retain recent PCM for device-buffer rollback
                r = StereoRing(cap)
            out = _instantiate(name, r, rate, blocksize, latency_ms, device, speed)
            out.start()
            if log:
                extra = f" @ {out.samplerate} Hz" if name != "null" else f" @ {out.samplerate} Hz (silent)"
                log(f"Audio output: {name} ({out.description}){extra}")
            return out, r
        except Exception as exc:
            if out is not None:
                try:
                    out.stop()
                except Exception:
                    pass
            errors.append(f"{name}: {exc}")
            if prefer not in ("auto", "", None):
                raise AudioError(f"Backend '{prefer}' could not be opened: {exc}") from exc
    raise AudioError("No audio backend available: " + ", ".join(errors))
