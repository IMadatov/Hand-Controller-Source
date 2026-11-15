/// A [HandControllerEngine] that invents its own data.
///
/// It is not a stub that returns constants. It runs the real decision the
/// engine runs — gesture transitions into `began`/`held`/`ended`, then
/// bindings matched against the hand that made them — so the UI is exercised
/// by the same shapes of data it will get from C++, including the awkward
/// ones: an unlabelled hand, a hand leaving the frame mid-gesture, and a
/// cursor command firing on every single frame.
library;

import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';

import '../domain/bindings.dart';
import '../domain/command.dart';
import '../domain/gesture.dart';
import '../domain/observation.dart';
import 'default_bindings.dart';
import 'engine.dart';
import 'mock_hand.dart';

/// One scripted pose and how long it lasts.
typedef _Beat = (GestureKind, int frames);

class MockEngine implements HandControllerEngine {
  MockEngine({
    this.frameInterval = const Duration(milliseconds: 35),
    this.simulateSinkUnavailable = true,
    DateTime Function()? clock,
  }) : _now = clock ?? DateTime.now;

  /// Where the timestamps on [CommandRecord] come from.
  ///
  /// Injected rather than read, which is the same rule the C++ recognizer
  /// follows: it never reads a clock it was not handed. Here it also makes
  /// the whole engine deterministic, so a rendered screenshot of the command
  /// log is the same on every run.
  final DateTime Function() _now;

  /// ~28 fps, which is roughly what the mockups show.
  final Duration frameInterval;

  /// When true, asking for input injection fails the way XTest fails on a
  /// Wayland session: the request is accepted and nothing moves.
  ///
  /// Defaults to true because that is the state of the machine this was built
  /// on, and because the failure is silent — a UI developed only against a
  /// working sink would never show the user why nothing happens.
  final bool simulateSinkUnavailable;

  final _state = ValueNotifier<EngineState>(EngineState.initial);
  final _frames = StreamController<EngineFrame>.broadcast();
  final _commands = StreamController<CommandRecord>.broadcast();

  Timer? _timer;
  int _frame = 0;
  var _bindings = mockDefaultBindings();
  bool _injectRequested = false;

  /// The pose each track held on the previous frame, so transitions can be
  /// derived rather than scripted. Keyed by track id, as the C++ recognizer
  /// keys its latches.
  final _previousGesture = <int, GestureKind>{};

  /// The handedness latched when the current gesture began. The label can
  /// flip mid-track; the binding must not follow it.
  final _latchedHandedness = <int, Handedness>{};

  final _previousCenter = <int, NormPoint>{};

  @override
  ValueListenable<EngineState> get state => _state;

  @override
  Stream<EngineFrame> get frames => _frames.stream;

  @override
  Stream<CommandRecord> get commands => _commands.stream;

  @override
  Future<List<CameraDevice>> enumerateCameras() async {
    // The paths the mockups show. There is no camera on the build machine,
    // so even the real implementation will often return an empty list — a
    // case the UI has to render.
    await Future<void>.delayed(const Duration(milliseconds: 120));
    return const [
      CameraDevice(
          id: 'v0', name: 'Integrated Webcam (HD)', path: '/dev/video0'),
      CameraDevice(
          id: 'v2', name: 'External USB Camera', path: '/dev/video2'),
    ];
  }

  @override
  Future<void> start({CameraDevice? camera, bool injectInput = false}) async {
    _state.value = _state.value.copyWith(
      lifecycle: EngineLifecycle.starting,
      camera: camera,
      fault: null,
    );

    // Opening a camera and loading two models is not instant, and a UI that
    // never sees `starting` will have no spinner when it matters.
    await Future<void>.delayed(const Duration(milliseconds: 450));

    _injectRequested = injectInput;
    _frame = 0;
    _previousGesture.clear();
    _latchedHandedness.clear();
    _previousCenter.clear();

    _state.value = _state.value.copyWith(
      lifecycle: EngineLifecycle.running,
      camera: camera ?? (await enumerateCameras()).firstOrNull,
    );
    _applySinkStatus();

    _timer = Timer.periodic(frameInterval, (_) => _tick());
  }

  @override
  Future<void> stop() async {
    _timer?.cancel();
    _timer = null;
    _state.value = _state.value.copyWith(
      lifecycle: EngineLifecycle.stopped,
      sink: SinkStatus.observeOnly,
      sinkReason: null,
    );
  }

  @override
  Future<void> setPaused(bool paused) async {
    if (!_state.value.lifecycle.isLive) return;
    _state.value = _state.value.copyWith(
      lifecycle: paused ? EngineLifecycle.paused : EngineLifecycle.running,
    );
  }

  @override
  Future<void> setInjectInput(bool inject) async {
    _injectRequested = inject;
    _applySinkStatus();
  }

  /// Asking to inject is not the same as succeeding, and the gap between the
  /// two is the single most dangerous fact in this project.
  void _applySinkStatus() {
    if (!_injectRequested) {
      _state.value = _state.value.copyWith(
        sink: SinkStatus.observeOnly,
        sinkReason: null,
      );
      return;
    }
    if (simulateSinkUnavailable) {
      _state.value = _state.value.copyWith(
        sink: SinkStatus.unavailable,
        sinkReason: 'This session is Wayland. XTest reports success and does '
            'nothing, so input is not being sent.',
      );
      return;
    }
    _state.value = _state.value.copyWith(
      sink: SinkStatus.active,
      sinkReason: null,
    );
  }

  @override
  Future<BindingSet> loadBindings() async {
    await Future<void>.delayed(const Duration(milliseconds: 80));
    return _bindings;
  }

  @override
  Future<void> saveBindings(BindingSet set) async {
    await Future<void>.delayed(const Duration(milliseconds: 120));

    // The engine validates independently of the editor. The editor should
    // have caught all of this already; if it did not, this is what the user
    // would otherwise have discovered as a stuck key.
    final problems = set.problems.map((p) => p.message).toList();
    if (problems.isNotEmpty) throw BindingsRejected(problems);

    _bindings = set;
  }

  @override
  Future<void> dispose() async {
    await stop();
    await _frames.close();
    await _commands.close();
    _state.dispose();
  }

  // --- The frame loop ------------------------------------------------------

  /// The right hand's script: point to move, click, point, pinch to double
  /// click, open palm to scroll, then hands down.
  static const _rightScript = <_Beat>[
    (GestureKind.none, 10),
    (GestureKind.pointIndex, 55),
    (GestureKind.twoFingers, 14),
    (GestureKind.pointIndex, 40),
    (GestureKind.pinch, 14),
    (GestureKind.openPalm, 45),
    (GestureKind.none, 20),
  ];

  /// The left hand's script, running at a different length to the right so
  /// the two drift in and out of phase. That overlap is the acceptance
  /// criterion: an arrow held with the left hand while the right moves the
  /// cursor.
  static const _leftScript = <_Beat>[
    (GestureKind.none, 25),
    (GestureKind.thumbUp, 38),
    (GestureKind.none, 12),
    (GestureKind.pointIndex, 44),
    (GestureKind.none, 16),
    (GestureKind.fourFingers, 30),
    (GestureKind.pinkyOnly, 26),
    (GestureKind.none, 30),
  ];

  static GestureKind _poseAt(List<_Beat> script, int frame) {
    final total = script.fold<int>(0, (a, b) => a + b.$2);
    var at = frame % total;
    for (final beat in script) {
      if (at < beat.$2) return beat.$1;
      at -= beat.$2;
    }
    return GestureKind.none;
  }

  /// True while the right hand is deliberately out of frame.
  ///
  /// A hand leaving mid-gesture is the most likely way this project hurts
  /// someone — a held key that is never released — so the mock produces it on
  /// a schedule rather than leaving it to chance.
  bool _rightOffScreen(int frame) => (frame % 520) >= 480;

  void _tick() {
    _frame++;
    final t = _frame * frameInterval.inMilliseconds / 1000.0;
    final paused = _state.value.lifecycle == EngineLifecycle.paused;

    final hands = <HandObservation>[];
    final events = <GestureEvent>[];
    final live = <int>{};

    void observe({
      required int trackId,
      required Handedness handedness,
      required double handednessScore,
      required GestureKind gesture,
      required NormPoint center,
      required double size,
      required double tilt,
    }) {
      live.add(trackId);
      hands.add(HandObservation(
        trackId: trackId,
        handedness: handedness,
        handednessScore: handednessScore,
        landmarks: buildHandLandmarks(
          center: center,
          size: size,
          gesture: gesture,
          mirror: handedness == Handedness.left,
          tilt: tilt,
        ),
        palmCenter: center,
        palmSize: size,
        gesture: gesture,
        gestureScore: gesture == GestureKind.none
            ? 0
            : 0.78 + 0.18 * math.sin(t * 1.7 + trackId),
      ));

      final previous = _previousCenter[trackId];
      final delta = previous == null
          ? (x: 0.0, y: 0.0)
          : (x: center.x - previous.x, y: center.y - previous.y);
      _previousCenter[trackId] = center;

      events.addAll(_transitions(
        trackId: trackId,
        handedness: handedness,
        gesture: gesture,
        position: center,
        delta: delta,
      ));
    }

    // --- Right hand: a slow figure across the right of the frame ---------
    if (!_rightOffScreen(_frame)) {
      // Handedness confidence dips, and when it dips far enough the label
      // becomes `unknown`. Phase 1 measured 0.575 on a real two-hand frame.
      final score = 0.72 + 0.23 * math.sin(t * 0.31);
      observe(
        trackId: 1,
        handedness: score < 0.6 ? Handedness.unknown : Handedness.right,
        handednessScore: score,
        gesture: _poseAt(_rightScript, _frame),
        center: (
          x: 0.63 + 0.11 * math.sin(t * 0.63),
          y: 0.50 + 0.13 * math.sin(t * 0.41 + 1.1),
        ),
        size: 0.15,
        tilt: 0.10 * math.sin(t * 0.5),
      );
    }

    // --- Left hand ---------------------------------------------------------
    observe(
      trackId: 2,
      handedness: Handedness.left,
      handednessScore: 0.88 + 0.08 * math.sin(t * 0.27),
      gesture: _poseAt(_leftScript, _frame),
      center: (
        x: 0.29 + 0.05 * math.sin(t * 0.47 + 2.2),
        y: 0.54 + 0.07 * math.sin(t * 0.35),
      ),
      size: 0.145,
      tilt: -0.08 * math.sin(t * 0.44),
    );

    // A track that disappeared gets an `ended` for whatever it was holding.
    // Without this the key stays down forever, which is the failure this
    // whole branch of the design exists to prevent.
    for (final trackId in _previousGesture.keys.toList()) {
      if (live.contains(trackId)) continue;
      final held = _previousGesture.remove(trackId)!;
      final handedness =
          _latchedHandedness.remove(trackId) ?? Handedness.unknown;
      _previousCenter.remove(trackId);
      if (held == GestureKind.none) continue;
      events.add(GestureEvent(
        trackId: trackId,
        handedness: handedness,
        kind: held,
        phase: GesturePhase.ended,
        position: (x: 0.0, y: 0.0),
        delta: (x: 0.0, y: 0.0),
        timestampUs: _frame * frameInterval.inMicroseconds,
      ));
    }

    _frames.add(EngineFrame(
      hands: hands,
      metrics: EngineMetrics(
        // Jittered, because a perfectly steady number looks fake and hides
        // whether the readout is actually updating.
        fps: 1000 / frameInterval.inMilliseconds + 1.4 * math.sin(t * 2.3),
        latencyMs: 11 + 2.5 * math.sin(t * 3.1),
        droppedFrames: _frame ~/ 400,
      ),
      timestampUs: _frame * frameInterval.inMicroseconds,
    ));

    if (paused) return;
    for (final event in events) {
      _dispatch(event);
    }
  }

  /// Derives `began` / `held` / `ended` from this frame's pose and the last.
  ///
  /// This mirrors the C++ recognizer's contract rather than its internals:
  /// no hysteresis, no minimum duration, no latch timing. It produces the
  /// right SHAPE of event stream, which is all the UI consumes.
  List<GestureEvent> _transitions({
    required int trackId,
    required Handedness handedness,
    required GestureKind gesture,
    required NormPoint position,
    required NormPoint delta,
  }) {
    final previous = _previousGesture[trackId] ?? GestureKind.none;
    _previousGesture[trackId] = gesture;

    final out = <GestureEvent>[];
    final timestampUs = _frame * frameInterval.inMicroseconds;

    GestureEvent make(GestureKind kind, GesturePhase phase, Handedness h) =>
        GestureEvent(
          trackId: trackId,
          handedness: h,
          kind: kind,
          phase: phase,
          position: position,
          delta: phase == GesturePhase.held ? delta : (x: 0.0, y: 0.0),
          timestampUs: timestampUs,
        );

    if (gesture == previous) {
      if (gesture != GestureKind.none) {
        // Held events carry the LATCHED label, not this frame's.
        out.add(make(gesture, GesturePhase.held,
            _latchedHandedness[trackId] ?? handedness));
      }
      return out;
    }

    if (previous != GestureKind.none) {
      out.add(make(previous, GesturePhase.ended,
          _latchedHandedness[trackId] ?? handedness));
    }
    if (gesture != GestureKind.none) {
      _latchedHandedness[trackId] = handedness;
      out.add(make(gesture, GesturePhase.began, handedness));
    } else {
      _latchedHandedness.remove(trackId);
    }
    return out;
  }

  /// Matches an event against the bindings and publishes what it produced.
  void _dispatch(GestureEvent event) {
    for (final binding in _bindings.bindings) {
      if (!binding.enabled) continue;
      if (binding.gesture != event.kind) continue;
      if (binding.phase != event.phase) continue;
      if (!binding.hand.matches(event.handedness)) continue;

      var command = binding.command;
      if (command.kind == CommandKind.moveCursor ||
          command.kind == CommandKind.scroll) {
        command = command.copyWith(delta: (
          x: event.delta.x * _bindings.sensitivity,
          y: event.delta.y * _bindings.sensitivity,
        ));
      }

      _commands.add(CommandRecord(
        command: command,
        handedness: event.handedness,
        gesture: event.kind,
        phase: event.phase,
        at: _now(),

        // Produced either way. Whether anything moved is a different
        // question, and conflating the two is how a UI ends up claiming to
        // work on a session where XTest silently does nothing.
        delivered: _state.value.sink == SinkStatus.active,
      ));
    }
  }
}
