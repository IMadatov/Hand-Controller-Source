/// The boundary between this UI and the C++ engine.
///
/// Shaped after the `Controller` the phase 2 plan calls for — `Start`,
/// `Stop`, `PollObservations`, `PollCommands` — so that the FFI
/// implementation can drop in behind this interface without a single widget
/// changing. The UI must never learn which implementation it holds.
///
/// Polling is adapted to streams here rather than exposed: a Flutter widget
/// rebuilding from a stream is idiomatic, and the FFI implementation will
/// poll on a timer and publish into these streams.
library;

import 'package:flutter/foundation.dart';

import '../domain/bindings.dart';
import '../domain/command.dart';
import '../domain/gesture.dart';
import '../domain/observation.dart';

/// Whether the pipeline is running.
enum EngineLifecycle {
  /// Not started. No camera held, no frames.
  stopped('Stopped'),

  /// Starting: camera opening, models loading.
  starting('Starting…'),

  /// Running and emitting frames.
  running('Active'),

  /// Still holding the camera, but emitting no commands.
  ///
  /// Distinct from [stopped] because resuming is instant and because a user
  /// who paused still wants to see the preview.
  paused('Paused'),

  /// Failed to start. The reason is in [EngineState.fault].
  failed('Failed');

  const EngineLifecycle(this.label);

  final String label;

  bool get isLive => this == running || this == paused;
}

/// Everything about the engine that is not per-frame.
@immutable
class EngineState {
  const EngineState({
    required this.lifecycle,
    required this.sink,
    this.sinkReason,
    this.fault,
    this.camera,
  });

  final EngineLifecycle lifecycle;

  /// Whether input is really being injected. Never inferred from commands
  /// having been sent — XTest reports success and does nothing on Wayland.
  final SinkStatus sink;

  /// Why [sink] is not active, when it is not. Shown verbatim to the user.
  final String? sinkReason;

  /// Why the engine failed to start, when it did.
  final String? fault;

  final CameraDevice? camera;

  static const initial = EngineState(
    lifecycle: EngineLifecycle.stopped,
    sink: SinkStatus.observeOnly,
  );

  EngineState copyWith({
    EngineLifecycle? lifecycle,
    SinkStatus? sink,
    String? sinkReason,
    String? fault,
    CameraDevice? camera,
  }) =>
      EngineState(
        lifecycle: lifecycle ?? this.lifecycle,
        sink: sink ?? this.sink,
        sinkReason: sinkReason ?? this.sinkReason,
        fault: fault ?? this.fault,
        camera: camera ?? this.camera,
      );
}

/// One frame's output: what was seen, and what it cost.
@immutable
class EngineFrame {
  const EngineFrame({
    required this.hands,
    required this.metrics,
    required this.timestampUs,
  });

  /// Zero, one or two hands.
  ///
  /// An empty list is a NORMAL state, not an error. The user simply has their
  /// hands down, and the UI must render that calmly rather than as a fault.
  final List<HandObservation> hands;

  final EngineMetrics metrics;
  final int timestampUs;
}

/// A command the mapper produced, with the context needed to explain it.
///
/// The engine returns commands as data whether or not a sink is attached —
/// the plan guarantees a sink-less controller is fully usable — so this is
/// what the command log renders in both modes.
@immutable
class CommandRecord {
  const CommandRecord({
    required this.command,
    required this.handedness,
    required this.gesture,
    required this.phase,
    required this.at,
    required this.delivered,
  });

  final HcCommand command;

  /// Which hand caused it. The log is far less useful without this, since the
  /// acceptance criterion is two hands acting independently.
  final Handedness handedness;

  final GestureKind gesture;
  final GesturePhase phase;
  final DateTime at;

  /// Whether a sink actually accepted it, as opposed to it having been
  /// produced and discarded. False in observe-only mode.
  final bool delivered;
}

/// The engine, as this UI sees it.
abstract interface class HandControllerEngine {
  /// Non-per-frame state. Listenable so the header and the status pill can
  /// rebuild without touching the frame stream.
  ValueListenable<EngineState> get state;

  /// Per-frame observations. Broadcast: the preview, the hand cards and the
  /// metrics readout all listen.
  Stream<EngineFrame> get frames;

  /// Commands as they are produced.
  Stream<CommandRecord> get commands;

  /// Cameras the engine can open.
  Future<List<CameraDevice>> enumerateCameras();

  /// Open [camera] (or the default) and begin tracking.
  ///
  /// [injectInput] false selects observe-only: commands are still produced
  /// and reported, and nothing is moved. This is a supported configuration,
  /// and the safe default for a first run.
  Future<void> start({CameraDevice? camera, bool injectInput = false});

  Future<void> stop();

  /// Keep the camera, stop emitting commands.
  Future<void> setPaused(bool paused);

  /// Switch input injection on or off while running.
  ///
  /// May leave [EngineState.sink] at [SinkStatus.unavailable] if the platform
  /// cannot inject — asking is not the same as succeeding.
  Future<void> setInjectInput(bool inject);

  /// Read the bindings.
  ///
  /// Parsing belongs to C++ `ParseBindings`; this hands back the result. The
  /// UI never reads the file itself.
  Future<BindingSet> loadBindings();

  /// Write the bindings through C++ `SerializeBindings`.
  ///
  /// Throws [BindingsRejected] if the engine refuses the set, which is the
  /// backstop behind the editor's own validation rather than the first line
  /// of defence.
  Future<void> saveBindings(BindingSet set);

  /// Release everything. The engine is unusable afterwards.
  Future<void> dispose();
}

/// Thrown when the engine rejects a configuration.
class BindingsRejected implements Exception {
  BindingsRejected(this.problems);

  /// The engine's complaints, in its own words.
  final List<String> problems;

  @override
  String toString() => 'Bindings rejected: ${problems.join('; ')}';
}
