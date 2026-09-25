"""Render modules on a worker thread, feed the audio output, and recover stalled playback. Only the
worker accesses its module handle."""

from __future__ import annotations

import copy
import math
import queue
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

import numpy as np

from .audio import AudioError, OutputDevice, StereoRing, create_output
from .config import normalise_sample_rate
from .openmpt import (DEFAULT_INTERPOLATION, Module, ModuleInfo, get_lib,
                      interpolation_length, interpolation_name)

# messages that are interesting for the UI
MSG_INFO, MSG_WARN, MSG_ERROR, MSG_DEBUG = "info", "warn", "error", "debug"

@dataclass
class Snapshot:
    """Immutable-ish view of the player state (read by the UI at ~12 Hz)."""

    generation: int = 0
    play_id: int = 0               # explicit load/replay identity; recovery keeps it
    audio_seconds: float = 0.0     # actual rendering, never advanced by a seek
    path: str = ""
    loading: bool = False
    loaded: bool = False
    playing: bool = False
    paused: bool = False
    ended: bool = False
    finished: bool = False          # song ended, buffered audio has drained
    failed: str = ""                # load error for the current track
    loop: bool = False
    position: float = 0.0
    duration: float = 0.0
    duration_valid: bool = False
    loop_index: int = 0
    order: int = 0
    pattern: int = 0
    row: int = 0
    speed: int = 0
    tempo: int = 0
    num_channels: int = 0
    playing_channels: int = 0
    subsong: int = 0
    num_subsongs: int = 0
    info: Optional[ModuleInfo] = None
    vu: tuple[float, ...] = ()
    level: tuple[float, float] = (0.0, 0.0)
    buffer_seconds: float = 0.0
    interpolation: int = 0          # filter length the mixer is resampling with
    stall_seconds: float = 0.0
    stalled: bool = False
    progress_time: float = 0.0      # monotonic timestamp of the last rendered block
    rendered_seconds: float = 0.0
    restarts: int = 0

@dataclass
class Message:
    level: str
    text: str
    time: float = field(default_factory=time.time)


class _Abandoned(Exception):
    pass


class _Worker(threading.Thread):
    def __init__(self, engine: "PlaybackEngine", generation: int, path: str = "",
                 position: float = 0.0, paused: bool = False):
        super().__init__(name=f"render-{generation}", daemon=True)
        self.engine = engine
        self.generation = generation
        self.initial_path = path
        self.initial_position = position
        self.initial_paused = paused
        self.cmds: queue.Queue = queue.Queue()
        self._state_lock = threading.Lock()
        self.state = Snapshot(generation=generation)
        self._abandon = threading.Event()
        self._module: Optional[Module] = None
        self._buf = np.zeros((engine.blocksize, 2), dtype=np.float32)
        self._gain = 0.0
        self._fade_frames = 0
        self._key = None
        self._key_frames = 0
        self._silent_frames = 0
        self._frames = 0            # rendered frames (audio clock for this song)
        self._audio_seconds = 0.0   # independent telemetry for listening stats
        self._stall_reported = False
        self._overrun_reported = False
        self._finished_reported = False
        self._load_started = 0.0
        self.level = np.zeros(2, dtype=np.float32)
        self._vu_channels = 0

    def publish(self, **fields) -> None:
        with self._state_lock:
            for key, value in fields.items():
                setattr(self.state, key, value)

    def snapshot(self) -> Snapshot:
        with self._state_lock:
            return copy.copy(self.state)

    def abandon(self) -> None:
        self._abandon.set()

    def post(self, name: str, **kwargs) -> None:
        self.cmds.put((name, kwargs))

    def _check_alive(self) -> None:
        if self._abandon.is_set() or self.engine._shutdown.is_set():
            raise _Abandoned()

    def _close_module(self) -> None:
        if self._module is not None:
            try:
                self._module.close()
            except Exception:
                pass
            self._module = None

    def _emit(self, level: str, text: str) -> None:
        self.engine._message(level, text)

    def run(self) -> None:  # noqa: C901 - a state machine, kept together on purpose
        self._load_started = time.monotonic()
        try:
            if self.initial_path:
                self._do_load(
                    self.initial_path,
                    position=self.initial_position,
                    start_paused=self.initial_paused,
                    loop=self.engine.settings.loop_track,
                )
            while True:
                self._check_alive()
                self._drain_commands()
                self._check_alive()
                if self._module is None:
                    self._idle(0.02)
                    continue
                state = self.snapshot()
                if state.failed:
                    self._idle(0.05)
                    continue
                if state.paused:
                    self._idle(0.02)
                    continue
                if state.ended:
                    self._wait_for_drain()
                    continue
                self._render()
        except _Abandoned:
            pass
        except Exception as exc:  # never let the thread die silently
            self._emit(MSG_ERROR, f"render worker stopped: {exc!r}")
            self.publish(playing=False)
        finally:
            self._close_module()

    def _idle(self, seconds: float) -> None:
        try:
            name, kwargs = self.cmds.get(timeout=seconds)
        except queue.Empty:
            return
        self._apply_command(name, kwargs)

    def _describe(self, kind: str, kwargs: dict) -> None:
        """Read song or pattern data on the render thread and return it to the UI."""
        module = self._module
        if module is None:
            return
        token = int(kwargs.get("token", 0))
        try:
            if kind == "song":
                orders = module.order_list()
                channels = module.num_channels()
                rows = {pattern: module.pattern_rows(pattern)
                        for pattern in set(orders) if pattern >= 0}
                snapshot = self.snapshot()
                self.engine._notify("song", token=token, orders=orders,
                                    channels=channels, rows=rows,
                                    num_patterns=module.num_patterns(),
                                    path=snapshot.path,
                                    format_name=(snapshot.info.format if snapshot.info else ""))
            else:
                pattern = int(kwargs["pattern"])
                data = module.pattern_data(pattern, channels=kwargs.get("channels"))
                self.engine._notify("pattern", token=token, **data)
        except Exception as exc:                      # never kill the render worker
            self.engine._notify("pattern", token=token, pattern=int(kwargs.get("pattern", -1)),
                                error=str(exc), cells=[])

    def _drain_commands(self) -> None:
        while True:
            try:
                name, kwargs = self.cmds.get_nowait()
            except queue.Empty:
                return
            self._apply_command(name, kwargs)

    def _apply_command(self, name: str, kwargs: dict) -> None:
        if name == "load":
            self._do_load(**kwargs)
        elif name == "listening_start":
            self._audio_seconds = 0.0
            self.publish(play_id=kwargs["play_id"], audio_seconds=0.0)
        elif name == "pause":
            if kwargs.get("paused"):
                self.engine._pause_output()
                self.publish(paused=True, playing=False, vu=(), level=(0.0, 0.0))
            else:
                self._gain = self.engine.effective_gain()
                self.engine._resume_output()
                self.publish(paused=False, playing=not self.snapshot().ended,
                             progress_time=time.monotonic())
        elif name == "seek":
            self._do_seek(float(kwargs["position"]))
        elif name == "loop":
            self._set_loop(bool(kwargs["loop"]))
        elif name == "subsong":
            self._do_subsong(int(kwargs["index"]))
        elif name == "tempo":
            try:
                if self._module is not None:
                    self._module.set_tempo_factor(float(kwargs["factor"]))
            except Exception as exc:
                self._emit(MSG_WARN, f"tempo factor ignored: {exc}")
        elif name == "interpolation":
            self._apply_interpolation(str(kwargs["mode"]))
        elif name in ("song", "pattern"):
            self._describe(name, kwargs)
        elif name == "unload":
            self._close_module()
            self.engine.ring.clear()
            self.publish(loaded=False, playing=False, paused=False, ended=False,
                         path="", info=None, position=0.0, duration=0.0,
                         duration_valid=False, failed="", vu=(), level=(0.0, 0.0))
        elif name == "quit":
            self._close_module()
            self._abandon.set()

    def _do_load(self, path: str, position: float = 0.0, start_paused: bool = False,
                 loop: Optional[bool] = None, subsong: int = 0, play_id=None) -> None:
        lib = get_lib()
        self._audio_seconds = 0.0
        self.publish(play_id=self.engine._play_serial if play_id is None else play_id,
                     audio_seconds=0.0)
        self._close_module()
        self.engine.ring.clear()
        self._gain = 0.0
        self._fade_frames = int(self.engine.samplerate * 0.015)
        self._key = None
        self._key_frames = 0
        self._silent_frames = 0
        self._frames = 0
        self._stall_reported = False
        self._overrun_reported = False
        self._finished_reported = False
        self._load_started = time.monotonic()
        if loop is not None:
            self.publish(loop=loop)
        self.publish(
            path=path, loading=True, loaded=False, playing=False, paused=start_paused,
            ended=False, finished=False, failed="", info=None, position=0.0, duration=0.0,
            duration_valid=False, loop_index=0, order=0, pattern=0, row=0, speed=0, tempo=0,
            vu=(), level=(0.0, 0.0), stall_seconds=0.0, stalled=False, rendered_seconds=0.0,
            subsong=subsong,
        )
        try:
            mod = lib.open_file(path)
        except Exception as exc:
            text = str(exc) or exc.__class__.__name__
            self.publish(loading=False, loaded=False, playing=False, failed=text,
                         info=None, path=path)
            self._emit(MSG_ERROR, f"cannot load {path}: {text}")
            self.engine._notify("load_failed", path=path, error=text, generation=self.generation)
            return
        self._module = mod
        try:
            duration = mod.duration()
            if subsong:
                try:
                    mod.select_subsong(subsong)
                    duration = mod.duration()
                except Exception:
                    pass
            mod.set_repeat_count(-1 if self.snapshot().loop else 0)
            try:
                # Keep EOF stopped; seeking explicitly starts another playback.
                mod.set_at_end("stop")
            except Exception:
                pass
            mode = self.engine.settings.interpolation
            if interpolation_length(mode) is None:
                self._emit(MSG_WARN, f"unknown resampling filter {mode!r} in the settings - "
                                     f"using {DEFAULT_INTERPOLATION}")
                mode = DEFAULT_INTERPOLATION
            try:
                mod.set_interpolation(mode)
            except Exception as exc:
                self._emit(MSG_WARN, f"resampling filter not applied: {exc}")
            info = mod.info(path)
            if position > 0:
                mod.seek_seconds(position)
            num_ch = max(1, info.num_channels)
            self._vu_channels = min(num_ch, 64)
            self.publish(
                loading=False, loaded=True, playing=not start_paused, paused=start_paused,
                info=info, duration=duration, duration_valid=bool(math.isfinite(duration) and duration > 0),
                position=mod.position_seconds(), num_channels=num_ch,
                num_subsongs=max(1, info.num_subsongs), subsong=subsong,
                speed=mod.current_speed(), tempo=mod.current_tempo(),
                interpolation=mod.interpolation(),
                order=mod.current_order(), pattern=mod.current_pattern(), row=mod.current_row(),
            )
            self._emit(MSG_INFO, f"loaded {path} ({info.format or '?'}, "
                                 f"{info.num_channels}ch, {duration:.1f}s)")
        except Exception as exc:
            text = f"{exc!r}"
            self.publish(loading=False, loaded=True, playing=False, failed=text)
            self._emit(MSG_ERROR, f"module failed after load: {text}")
        if start_paused:
            self.engine.ring.pause()
        else:
            self.engine._resume_output()
        self._load_started = time.monotonic()

    def _apply_interpolation(self, mode: str, *, announce: bool = True) -> bool:
        """Apply the resampling filter to the current module without reloading it."""
        length = interpolation_length(mode)
        if length is None:
            self._emit(MSG_WARN, f"unknown resampling filter {mode!r} - keeping "
                                 f"{interpolation_name(self.snapshot().interpolation) or 'the default'}")
            return False
        if self._module is None:
            return False
        try:
            self._module.set_interpolation(length)
        except Exception as exc:
            self._emit(MSG_WARN, f"resampling filter unchanged: {exc}")
            return False
        actual = self._module.interpolation()
        self.publish(interpolation=actual)
        if announce:
            self._emit(MSG_INFO, f"resampling: {interpolation_name(actual) or mode} "
                                 f"({actual} tap filter)")
        return True

    def _set_loop(self, loop: bool) -> None:
        self.publish(loop=loop)
        if self._module is None:
            return
        try:
            self._module.set_repeat_count(-1 if loop else 0)
            self._emit(MSG_INFO, "loop " + ("on" if loop else "off"))
        except Exception as exc:
            self._emit(MSG_WARN, f"loop change failed: {exc}")

    def _do_subsong(self, index: int) -> None:
        if self._module is None:
            return
        try:
            self._module.select_subsong(index)
            self.engine.ring.clear()
            duration = self._module.duration()
            self._frames = 0
            self._key = None
            self._key_frames = 0
            self._stall_reported = False
            self.publish(subsong=index, duration=duration,
                         duration_valid=bool(math.isfinite(duration) and duration > 0),
                         position=self._module.position_seconds(), ended=False,
                         finished=False, loop_index=0)
            self._emit(MSG_INFO, f"subsong {index + 1}/{self.snapshot().num_subsongs}")
        except Exception as exc:
            self._emit(MSG_WARN, f"cannot select subsong {index}: {exc}")

    def _do_seek(self, position: float) -> None:
        if self._module is None:
            return
        try:
            newpos = self._module.seek_seconds(max(0.0, position))
            self.engine.ring.clear()
            self._fade_frames = int(self.engine.samplerate * 0.012)
            self._gain = 0.0
            self._frames = int(newpos * self.engine.samplerate)
            self._key = None
            self._key_frames = self._frames
            self._silent_frames = 0
            self._stall_reported = False
            self._overrun_reported = False
            self._finished_reported = False
            st = self.snapshot()
            self.publish(position=newpos, ended=False, finished=False,
                         order=self._module.current_order(), row=self._module.current_row(),
                         playing=not st.paused)
        except Exception as exc:
            self._emit(MSG_WARN, f"seek failed: {exc}")

    def _wait_for_drain(self) -> None:
        """Song is over: wait until the buffered tail has been played."""
        if self.engine.ring.drained() and not self._finished_reported:
            self._finished_reported = True
            self.publish(finished=True, playing=False, paused=False, vu=(),
                         level=(0.0, 0.0))
            self.engine._notify(
                "finished", path=self.snapshot().path, duration=self.snapshot().duration,
                loop=self.snapshot().loop, generation=self.generation,
            )
            self._emit(MSG_INFO, "song finished")
        self._idle(0.02)

    def _render(self) -> None:  # noqa: C901
        ring: StereoRing = self.engine.ring
        block = self.engine.blocksize
        target = int(self.engine.samplerate * self.engine.buffer_ms / 1000.0)
        target = max(target, block * 2)
        available = ring.available()
        if available >= target or ring.space() < 64:
            self._idle(0.005)     # device is behind; wait instead of dropping audio
            return
        frames = min(block, target - available, ring.space())
        module = self._module
        state = self.snapshot()
        identity = (state.generation, state.play_id, state.path, state.subsong)
        before = (identity, state.order, state.pattern, state.row)
        n = module.read_interleaved_float(self.engine.samplerate, frames, self._buf)
        if n <= 0:
            self.publish(ended=True, playing=False)
            return
        data = self._buf[:n]

        target_gain = self.engine.effective_gain()
        if self._fade_frames > 0:
            f = min(self._fade_frames, n)
            data[:f] *= np.linspace(0.0, 1.0, f, endpoint=False, dtype=np.float32)[:, None]
            self._fade_frames -= f
        unscaled = data.copy()
        if abs(target_gain - self._gain) < 1e-4:
            if abs(target_gain - 1.0) > 1e-6:
                data *= target_gain
            self._gain = target_gain
        else:
            ramp = np.linspace(self._gain, target_gain, n, dtype=np.float32)[:, None]
            data *= ramp
            self._gain = target_gain

        peak = np.abs(self._buf[:n]).max(axis=0) if n else np.zeros(2, dtype=np.float32)
        # limit to a couple of pixels of work: no full copy needed
        self.level = np.maximum(peak, self.level * 0.82)

        self._frames += n
        self._audio_seconds += n / self.engine.samplerate
        order = module.current_order()
        pattern = module.current_pattern()
        row = module.current_row()
        position = module.position_seconds()
        ring.write(data, before=before, after=(identity, order, pattern, row),
                   position_before=state.position, position_after=position, unscaled=unscaled)
        speed = module.current_speed()
        tempo = module.current_tempo()
        key = (order, pattern, row, speed, tempo)
        if key != self._key:
            self._key = key
            self._key_frames = self._frames
            self._stall_reported = False
        silent = bool(max(peak) < 1e-4)
        if silent:
            self._silent_frames += n
        else:
            self._silent_frames = 0

        duration = self.snapshot().duration
        loop = self.snapshot().loop
        loop_index = int(position // duration) if (loop and math.isfinite(duration) and duration > 0) else 0

        vu = ()
        if self.engine.metering:
            vu = tuple(module.channel_vu(i) for i in range(self._vu_channels))

        self.publish(
            playing=True, paused=False, ended=False, position=position,
            audio_seconds=self._audio_seconds,
            order=order, pattern=pattern, row=row, speed=speed, tempo=tempo,
            playing_channels=module.playing_channels(), loop_index=loop_index,
            vu=vu,
            level=(float(self.level[0] * self._gain), float(self.level[1] * self._gain)),
            progress_time=time.monotonic(), rendered_seconds=self._frames / self.engine.samplerate,
        )

        if n < frames:
            # libopenmpt ran out of song mid-block -> end of song
            self.publish(ended=True, playing=False)

        self._watch_health(no_progress=(self._frames - self._key_frames) / self.engine.samplerate,
                           silent_seconds=self._silent_frames / self.engine.samplerate,
                           position=position, duration=duration, loop=loop)

    def _watch_health(self, no_progress: float, silent_seconds: float, position: float,
                      duration: float, loop: bool) -> None:
        """Detect modules that are playing but going nowhere / never ending."""
        cfg = self.engine.settings
        silent_stall = cfg.silence_stall_timeout
        hard_stall = cfg.stall_timeout
        stalled = False
        reason = ""
        if silent_stall > 0 and no_progress >= silent_stall and silent_seconds >= silent_stall:
            stalled, reason = True, f"no pattern progress while silent ({silent_seconds:.0f}s)"
        elif hard_stall > 0 and no_progress >= hard_stall:
            stalled, reason = True, f"no pattern progress for {no_progress:.0f}s"
        if stalled:
            self.publish(stalled=True, stall_seconds=no_progress)
            if not self._stall_reported:
                self._stall_reported = True
                self._emit(MSG_WARN, f"module stalled: {reason}")
                self.engine._notify("stalled", path=self.snapshot().path, reason=reason,
                                    position=position, generation=self.generation)
        else:
            self.publish(stalled=False, stall_seconds=no_progress)

        # a module that never ends even though libopenmpt knows its length
        if (
            (cfg.overrun_guard and not loop and math.isfinite(duration) and duration > 0
             and position > max(duration * 2.0, duration + 90.0))
            and (not self._overrun_reported)
        ):
            self._overrun_reported = True
            self._emit(MSG_WARN,
                       f"module did not end after {position:.0f}s (expected {duration:.0f}s)")
            self.engine._notify("overrun", path=self.snapshot().path, position=position,
                                duration=duration, generation=self.generation)


class PlaybackEngine:
    """Owns the audio device, the ring buffer and the current render worker."""

    def __init__(
        self,
        settings,
        backend: str = "auto",
        samplerate: Optional[int] = None,
        blocksize: int = 1024,
        speed: float = 1.0,
        device=None,
        metering: bool = True,
    ):
        self.settings = settings
        self.backend_name = backend
        self._samplerate = (int(samplerate) if samplerate is not None else
                            normalise_sample_rate(getattr(settings, "samplerate", 0))) or None
        self.blocksize = int(blocksize)
        self.speed = float(speed)
        self.device = device
        self.metering = bool(metering)

        self._shutdown = threading.Event()
        self._message_queue: queue.Queue = queue.Queue()
        self._event_queue: queue.Queue = queue.Queue()
        self._workers: dict[int, _Worker] = {}
        self._generation = 0
        self._play_serial = 0
        self._lock = threading.RLock()
        self._restarts: dict[str, int] = {}
        self._volume = max(0.0, min(1.0, settings.volume / 100.0))
        self._muted = bool(settings.muted)

        self.output: Optional[OutputDevice] = None
        self.ring: StereoRing
        self.samplerate: int
        self._open_output(ring=None)

        self._supervisor = threading.Thread(target=self._supervise, name="supervisor", daemon=True)
        self._supervisor.start()

    def _open_output(self, ring: Optional[StereoRing]) -> None:
        latency = float(getattr(self.settings, "latency_ms", 20))
        out, r = create_output(
            prefer=self.backend_name,
            ring=ring,
            samplerate=self._samplerate,
            blocksize=self.blocksize,
            latency_ms=latency,
            device=self.device,
            speed=self.speed,
            log=lambda text: self._message(MSG_INFO, text),
        )
        self.output, self.ring = out, r
        self.samplerate = out.samplerate
        self.buffer_ms = int(getattr(self.settings, "buffer_ms", 220))

    def _reopen_output(self) -> None:
        with self._lock:
            old_ring = self.ring
            try:
                self.output.stop()
            except Exception:
                pass
            try:
                new_ring = old_ring if old_ring.paused else StereoRing(5 * max(self.samplerate, 44100))
                self._open_output(ring=new_ring)
                self._message(MSG_WARN, f"audio device reopened ({self.output.name})")
            except AudioError as exc:
                self._message(MSG_ERROR, f"could not reopen audio device: {exc}")
                try:
                    self.ring = old_ring
                except Exception:
                    pass

    def _pause_output(self) -> None:
        with self._lock:
            try:
                self.output.pause()
            except Exception as exc:
                self._message(MSG_WARN, f"audio pause: {exc}")

    def _resume_output(self) -> None:
        with self._lock:
            try:
                self.output.resume()
            except Exception as exc:
                self._message(MSG_WARN, f"audio resume: {exc}")

    def restart_output(self, backend: Optional[str] = None) -> None:
        """Public: switch backend / recover from a dead sound card."""
        if backend:
            self.backend_name = backend
        self._reopen_output()

    def _message(self, level: str, text: str) -> None:
        self._message_queue.put(Message(level, text))

    def _notify(self, kind: str, **payload) -> None:
        self._event_queue.put((kind, payload))

    def request_song(self, token: int = 0) -> None:
        """Ask the render worker for the order list + pattern lengths."""
        worker = self._current()
        if worker is not None and worker.is_alive():
            worker.post("song", token=int(token))

    def request_pattern(self, pattern: int, channels: Optional[int] = None,
                        token: int = 0) -> None:
        """Ask the render worker for one pattern's cells."""
        worker = self._current()
        if worker is not None and worker.is_alive():
            worker.post("pattern", pattern=int(pattern), channels=channels, token=int(token))

    def poll_messages(self) -> list[Message]:
        out = []
        while True:
            try:
                out.append(self._message_queue.get_nowait())
            except queue.Empty:
                return out

    def poll_events(self) -> list[tuple[str, dict]]:
        out = []
        while True:
            try:
                out.append(self._event_queue.get_nowait())
            except queue.Empty:
                return out

    @property
    def volume(self) -> float:
        return self._volume

    def set_volume(self, value: float) -> None:
        self._volume = max(0.0, min(1.0, float(value)))
        self.ring.set_paused_gain(self.effective_gain())

    def set_muted(self, muted: bool) -> None:
        self._muted = bool(muted)
        self.ring.set_paused_gain(self.effective_gain())

    @property
    def muted(self) -> bool:
        return self._muted

    def effective_gain(self) -> float:
        if self._muted:
            return 0.0
        # perceptual-ish taper: the slider is linear in "loudness"
        return float(self._volume ** 2)

    def _current(self) -> Optional[_Worker]:
        with self._lock:
            return self._workers.get(self._generation)

    def _spawn(self, path: str = "", position: float = 0.0, paused: bool = False) -> _Worker:
        with self._lock:
            old = self._workers.pop(self._generation, None)
            if old is not None and old.is_alive():
                old.abandon()
            self._generation += 1
            worker = _Worker(self, self._generation, path=path, position=position, paused=paused)
            self._workers[self._generation] = worker
            worker.start()
            return worker

    def _supervise(self) -> None:
        """Watchdog: keep audio flowing even if a worker wedges or the device dies."""
        while not self._shutdown.wait(0.4):
            try:
                self._supervise_once()
            except Exception as exc:  # pragma: no cover - defensive
                self._message(MSG_ERROR, f"supervisor error: {exc!r}")

    def _supervise_once(self) -> None:
        out = self.output
        if out is not None and not out.is_running() and not self._shutdown.is_set():
            self._message(MSG_WARN, "audio device stopped; reopening")
            self._reopen_output()

        worker = self._current()
        if worker is None:
            return
        st = self.snapshot()
        now = time.monotonic()

        if not worker.is_alive():
            if not self._shutdown.is_set() and st.path and not st.finished:
                self._message(MSG_WARN, f"render thread died; restarting ({st.path})")
                worker.abandon()
                self._spawn(st.path, st.position, paused=True)
                self._notify("restarted", path=st.path, reason="thread died")
            return

        if st.loading and now - worker._load_started > max(20.0, self.settings.hang_timeout * 3):
            self._recover(worker, st, "module load hung")

        elif (
            (st.loaded and not st.paused and not st.ended and not st.finished and
             st.progress_time)
            and (now - st.progress_time > max(2.0, self.settings.hang_timeout))
        ):
            self._recover(worker, st, f"no audio for {now - st.progress_time:.1f}s")

        if st.path and st.rendered_seconds > 15.0:
            self._restarts.pop(st.path, None)

    def _recover(self, worker: _Worker, st: Snapshot, reason: str) -> None:
        path = st.path
        count = self._restarts.get(path, 0) + 1
        self._restarts[path] = count
        limit = max(1, int(self.settings.max_restarts))
        if count > limit:
            self._message(MSG_ERROR, f"{path}: giving up after {count - 1} restarts ({reason})")
            self._notify("track_broken", path=path, reason=reason, restarts=count - 1)
            worker.abandon()
            self._spawn("", 0.0, paused=True)
            return
        self._message(MSG_WARN, f"restarting playback ({reason}); attempt {count}/{limit}")
        worker.abandon()
        new_worker = self._spawn(path, st.position, paused=st.paused)
        new_worker.publish(restarts=count)
        self._notify("restarted", path=path, reason=reason, restarts=count)

    def snapshot(self) -> Snapshot:
        worker = self._current()
        if worker is None:
            return Snapshot(loop=bool(self.settings.loop_track))
        snap = worker.snapshot()
        if self.ring.paused and snap.loaded:
            snap.paused, snap.playing = True, False
            snap.vu, snap.level = (), (0.0, 0.0)
            identity = (snap.generation, snap.play_id, snap.path, snap.subsong)
            position = self.ring.paused_position()
            if position is not None and position[0] == identity:
                snap.position = position[1]
            mark = self.ring.playback_mark()
            if mark is not None and mark[0] == identity:
                snap.order, snap.pattern, snap.row = mark[1:]
        snap.buffer_seconds = self.ring.pending() / max(1, self.samplerate)
        snap.restarts = self._restarts.get(snap.path, 0)
        snap.loop = self.settings.loop_track
        return snap

    def tracker_snapshot(self, snap: Optional[Snapshot] = None) -> Snapshot:
        """Return output-timed row fields without changing the transport snapshot. Reject stale
        song or worker markers."""
        snap = copy.copy(snap) if snap is not None else self.snapshot()
        if not snap.loaded or snap.failed:
            return snap
        mark = self.ring.playback_mark()
        identity = (snap.generation, snap.play_id, snap.path, snap.subsong)
        if mark is not None and mark[0] == identity:
            snap.order, snap.pattern, snap.row = mark[1:]
        return snap

    def play_path(self, path: str, position: float = 0.0, paused: bool = False,
                  loop: Optional[bool] = None, subsong: int = 0) -> None:
        if loop is not None:
            self.settings.loop_track = bool(loop)
        with self._lock:
            self._play_serial += 1
            play_id = self._play_serial
        self._message(MSG_DEBUG, f"queue: {path}")
        worker = self._current()
        if worker is not None and worker.is_alive():
            worker.post("load", path=path, position=position, start_paused=paused,
                        loop=self.settings.loop_track, subsong=subsong, play_id=play_id)
        else:
            worker = self._spawn()
            worker.post("load", path=path, position=position, start_paused=paused,
                        loop=self.settings.loop_track, subsong=subsong, play_id=play_id)

    def unload(self) -> None:
        worker = self._current()
        if worker is not None:
            worker.post("unload")
            self.ring.clear()

    def play(self) -> None:
        worker = self._current()
        if worker is None:
            return
        st = self.snapshot()
        if st.finished or (st.ended and not st.paused):
            # the song is over: pressing play replays it from the top
            with self._lock:
                self._play_serial += 1
                play_id = self._play_serial
            worker.post("listening_start", play_id=play_id)
            worker.post("seek", position=0.0)
            worker.post("pause", paused=False)
            self._message(MSG_INFO, "replaying from the start")
        else:
            worker.post("pause", paused=False)

    def pause(self) -> None:
        with self._lock:
            worker = self._current()
            if worker is not None:
                worker.post("pause", paused=True)
                self._pause_output()

    def toggle_pause(self) -> None:
        st = self.snapshot()
        if not st.loaded:
            return
        if st.paused:
            self.play()
        else:
            self.pause()

    def is_playing(self) -> bool:
        return self.snapshot().playing

    def set_loop(self, loop: bool) -> None:
        self.settings.loop_track = bool(loop)
        worker = self._current()
        if worker is not None:
            worker.post("loop", loop=bool(loop))

    def seek(self, position: float) -> None:
        worker = self._current()
        if worker is not None:
            worker.post("seek", position=max(0.0, float(position)))

    def seek_relative(self, delta: float) -> None:
        st = self.snapshot()
        self.seek(st.position + float(delta))

    def seek_fraction(self, fraction: float) -> None:
        st = self.snapshot()
        duration = st.duration if st.duration_valid else None
        if duration:
            self.seek(max(0.0, min(1.0, float(fraction))) * duration)
        else:
            # unknown length: treat the slider as a relative nudge window
            self.seek(max(0.0, float(fraction) * max(st.position + 300.0, 300.0)))

    def select_subsong(self, index: int) -> None:
        worker = self._current()
        if worker is not None:
            worker.post("subsong", index=int(index))

    def set_tempo_factor(self, factor: float) -> None:
        worker = self._current()
        if worker is not None:
            worker.post("tempo", factor=float(factor))

    def set_interpolation(self, mode: str) -> bool:
        """Request a resampling filter change on the render thread."""
        if interpolation_length(mode) is None:
            self._message(MSG_WARN,
                          f"{mode!r} is not a resampling filter libopenmpt has - keeping "
                          f"{interpolation_name(self.snapshot().interpolation) or 'the current one'}")
            return False
        self.settings.interpolation = str(mode)
        worker = self._current()
        if worker is not None:
            worker.post("interpolation", mode=str(mode))
        return True

    def shutdown(self) -> None:
        self._shutdown.set()
        with self._lock:
            for worker in list(self._workers.values()):
                worker.abandon()
                try:
                    worker.post("quit")
                except Exception:
                    pass
        for worker in list(self._workers.values()):
            worker.join(timeout=0.6)
        if self.output is not None:
            try:
                self.output.stop()
            except Exception:
                pass

    def __enter__(self) -> "PlaybackEngine":
        return self

    def __exit__(self, *exc) -> None:
        self.shutdown()
