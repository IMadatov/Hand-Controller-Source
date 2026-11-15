/// What the tracker saw. Mirrors `hc::HandObservation` plus the few derived
/// values the UI displays beside it.
library;

import 'package:flutter/foundation.dart';

import 'gesture.dart';

/// The number of landmarks the hand model emits. Fixed by the model, not a
/// preference.
const int kLandmarkCount = 21;

/// A normalized point. `0..1` across the frame, origin top-left.
///
/// The core speaks normalized coordinates everywhere and exactly two places
/// multiply by pixels: the overlay that draws, and the sink that injects. A
/// pixel value arriving in this type means a layer leaked.
typedef NormPoint = ({double x, double y});

/// One hand, on one frame.
///
/// FIELD-FOR-FIELD the C++ `hc::HandObservation`, and deliberately nothing
/// more. It knows **nothing about gestures** — that is the recognizer's
/// output, delivered as `GestureEvent`s, and a `gesture` field here would be
/// a second source of truth that drifts from the first.
///
/// Palm centre and palm size are not here either. The recognizer derives
/// them; a `GestureEvent` carries the position. Anything that needs a palm
/// centre purely to draw computes it from [landmarks], which is a drawing
/// concern and stays in the painter.
@immutable
class HandObservation {
  const HandObservation({
    required this.trackId,
    required this.handedness,
    required this.handednessScore,
    required this.presence,
    required this.landmarks,
    required this.timestampUs,
  });

  /// Identity across frames. The gesture state machine keys on this, not on
  /// [handedness], because the handedness label can flip mid-track.
  final int trackId;

  final Handedness handedness;

  /// The model's confidence in [handedness], `0..1`.
  ///
  /// Phase 1 measured this as low as 0.575 on a two-hand frame — barely over
  /// the line. The UI must show it, because a user who cannot see a weak
  /// label cannot understand why the wrong hand's binding fired.
  final double handednessScore;

  /// How sure the tracker is that this is a hand at all, `0..1`.
  ///
  /// Distinct from [handednessScore], which is confidence in WHICH hand. A
  /// hand can be unmistakably present and still unlabelled.
  final double presence;

  /// [kLandmarkCount] points, normalized.
  final List<NormPoint> landmarks;

  final int timestampUs;
}

/// Whether the app can actually inject input, and why not when it cannot.
///
/// This exists because XTest fails SILENTLY: it returns success and does
/// nothing. A UI that reports "running" on the strength of commands having
/// been sent would lie on a Wayland session. The engine must report whether
/// the sink was really created, and the UI must show it.
enum SinkStatus {
  /// Injecting for real. Commands move the cursor.
  active('Input active'),

  /// Deliberately not injecting. Commands are returned as data and nothing
  /// is moved. A supported configuration, not a failure — it is the only
  /// sane mode for a server, and it is how a user tries gestures out safely.
  observeOnly('Tracking only — input not sent'),

  /// Wanted to inject and could not. The reason belongs beside this.
  unavailable('Input unavailable');

  const SinkStatus(this.label);

  final String label;
}

/// Where frames come from.
@immutable
class CameraDevice {
  const CameraDevice({
    required this.id,
    required this.name,
    required this.path,
  });

  final String id;
  final String name;

  /// e.g. `/dev/video0`. Shown because a user with two cameras picks by it.
  final String path;
}

/// A frame's worth of measured cost, for the header readout and the
/// diagnostics screen.
@immutable
class EngineMetrics {
  const EngineMetrics({
    required this.fps,
    required this.latencyMs,
    required this.droppedFrames,
  });

  final double fps;

  /// Wall time from frame received to commands emitted.
  final double latencyMs;

  /// Frames the pipeline skipped because it could not keep up. Surfaced
  /// rather than hidden: a smooth-looking 10 fps with half the frames
  /// dropped is a different problem from an honest 10 fps.
  final int droppedFrames;

  static const zero =
      EngineMetrics(fps: 0, latencyMs: 0, droppedFrames: 0);
}
