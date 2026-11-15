/// Mirrors `cpp/control/include/hc/gesture_event.hpp`.
///
/// The `wire` string on every value below is a PUBLISHED INTERFACE shared
/// with the C++ core — it is what appears in the bindings configuration file.
/// Renaming one breaks every saved configuration, so these strings are fixed
/// even where a nicer Dart name exists.
library;

import 'package:flutter/foundation.dart';

/// The gesture vocabulary.
///
/// Each name describes the POSE, never the action it drives. [twoFingers] is
/// bound to a left click by default, but the name does not say so, because
/// any pose can be bound to anything — a kind called `twoFingerClick` would
/// become a lie the moment a user rebound it.
enum GestureKind {
  /// Not a gesture, and never emitted in a [GestureEvent]. It exists so that
  /// "no gesture is active" is representable without a null, and is rejected
  /// as a binding target.
  none('none', 'None', 'None'),

  pinch('pinch', 'Thumb + index pinch', 'Pinch'),
  fist('fist', 'Fist', 'Fist'),
  openPalm('open_palm', 'Open palm', 'Open palm'),
  pointIndex('point_index', 'Index finger only', 'Index only'),
  twoFingers('two_fingers', 'Index + middle', 'Two fingers'),
  thumbUp('thumb_up', 'Thumb only', 'Thumb'),
  pinkyOnly('pinky_only', 'Pinky only', 'Pinky'),
  fourFingers('four_fingers', 'Four fingers, thumb folded', 'Four fingers');

  const GestureKind(this.wire, this.label, this.shortLabel);

  /// The name used in the bindings file. Do not reword.
  final String wire;

  /// What a user reads where there is room — the bindings editor, where the
  /// row is wide and the pose has to be unambiguous.
  final String label;

  /// For the narrow places: the hand cards and the tray overlay.
  ///
  /// A truncated label with an ellipsis tells the user nothing; a shorter
  /// one that fits tells them the pose. The long form is still available as
  /// a tooltip wherever this is used.
  final String shortLabel;

  /// The kinds a user may actually bind — everything except [none].
  static List<GestureKind> get bindable =>
      values.where((k) => k != GestureKind.none).toList(growable: false);

  static GestureKind? fromWire(String wire) {
    for (final k in values) {
      if (k.wire == wire) return k;
    }
    return null;
  }
}

/// The three phases of a gesture.
///
/// This is why the core reports transitions rather than a level: a design
/// emitting only "fist is true this frame" can express neither the one-shot
/// latch nor the release.
enum GesturePhase {
  /// The gesture just became active. One-shot bindings (click, screenshot)
  /// fire here, exactly once.
  began('began', 'on start'),

  /// Still active. Continuous bindings (cursor motion, scroll) act here, on
  /// every frame.
  held('held', 'while held'),

  /// Stopped — either the pose relaxed or the hand left the frame. A binding
  /// holding a key or a button releases here.
  ended('ended', 'on release');

  const GesturePhase(this.wire, this.label);

  final String wire;
  final String label;

  static GesturePhase? fromWire(String wire) {
    for (final p in values) {
      if (p.wire == wire) return p;
    }
    return null;
  }
}

/// Which hand the model thinks it saw.
///
/// [unknown] is NOT a user-facing choice — see `HandSelector`. It is a
/// statement about the model's confidence, and phase 1 measured that
/// confidence as low as 0.575, so it arrives often and must be rendered
/// without pretending to be a left or a right hand.
enum Handedness {
  left('left', 'Left'),
  right('right', 'Right'),
  unknown('unknown', 'Unlabelled');

  const Handedness(this.wire, this.label);

  final String wire;
  final String label;

  static Handedness? fromWire(String wire) {
    for (final h in values) {
      if (h.wire == wire) return h;
    }
    return null;
  }
}

/// One gesture transition for one tracked hand on one frame.
@immutable
class GestureEvent {
  const GestureEvent({
    required this.trackId,
    required this.handedness,
    required this.kind,
    required this.phase,
    required this.position,
    required this.delta,
    required this.timestampUs,
  });

  final int trackId;

  /// The handedness LATCHED AT [GesturePhase.began], not re-read per frame.
  ///
  /// The label can flip mid-track. If this followed the current observation,
  /// a flip halfway through a held gesture would hand the event to the other
  /// hand's binding — a left-hand gesture firing right-hand commands.
  final Handedness handedness;

  final GestureKind kind;
  final GesturePhase phase;

  /// Palm centre, frame-normalized `0..1`. On an [GesturePhase.ended] caused
  /// by the hand leaving the frame this is the last position observed.
  final ({double x, double y}) position;

  /// Movement since the previous frame this track was seen. Zero on
  /// [GesturePhase.began], and zero on an `ended` caused by hand loss.
  final ({double x, double y}) delta;

  final int timestampUs;
}
