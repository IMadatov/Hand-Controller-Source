/// Synthetic 21-point hands, so the preview and the overlay have something
/// to draw before a camera exists.
///
/// This is a drawing aid, not a model of anatomy. It exists to make the
/// overlay, the per-hand cards and the gesture readout developable and
/// reviewable; the moment `FfiEngine` lands, real landmarks replace it and
/// this file becomes test scaffolding.
library;

import 'dart:math' as math;

import '../domain/gesture.dart';
import '../domain/observation.dart';

/// Landmark indices, as the hand model orders them. Named because
/// `landmarks[8]` in a painter is unreadable.
abstract final class Lm {
  static const wrist = 0;

  /// Each finger is four consecutive points, base to tip.
  static const thumb = 1;
  static const index = 5;
  static const middle = 9;
  static const ring = 13;
  static const pinky = 17;

  /// Finger bases, in order, for drawing the palm outline.
  static const palmOutline = [wrist, index, middle, ring, pinky];

  /// The bone pairs to draw. Five chains plus the palm web.
  static List<(int, int)> get connections => [
        for (final base in [thumb, index, middle, ring, pinky])
          for (var j = 0; j < 3; j++) (base + j, base + j + 1),
        (wrist, thumb),
        (wrist, index),
        (wrist, pinky),
        (index, middle),
        (middle, ring),
        (ring, pinky),
      ];
}

/// How extended each finger is, thumb first, `0` folded to `1` straight.
typedef FingerCurl = ({
  double thumb,
  double index,
  double middle,
  double ring,
  double pinky,
});

/// The pose each gesture looks like.
///
/// These are the shapes the C++ recognizer scores from real landmarks. Here
/// the arrow runs backwards — pose chosen, landmarks generated — so the
/// numbers describe the drawing, and a mismatch with the recognizer's
/// thresholds is expected and harmless.
FingerCurl curlFor(GestureKind kind) => switch (kind) {
      GestureKind.openPalm =>
        (thumb: 1, index: 1, middle: 1, ring: 1, pinky: 1),
      GestureKind.fist => (thumb: 0, index: 0, middle: 0, ring: 0, pinky: 0),
      GestureKind.pointIndex =>
        (thumb: 0.1, index: 1, middle: 0, ring: 0, pinky: 0),
      GestureKind.twoFingers =>
        (thumb: 0.1, index: 1, middle: 1, ring: 0, pinky: 0),
      GestureKind.thumbUp =>
        (thumb: 1, index: 0, middle: 0, ring: 0, pinky: 0),
      GestureKind.pinkyOnly =>
        (thumb: 0.1, index: 0, middle: 0, ring: 0, pinky: 1),
      GestureKind.fourFingers =>
        (thumb: 0, index: 1, middle: 1, ring: 1, pinky: 1),

      // Thumb and index both out but bent toward each other; the tips meet
      // below. Deliberately NOT "both extended", which is the pose this
      // gesture was chosen to be distinguishable from.
      GestureKind.pinch =>
        (thumb: 0.72, index: 0.72, middle: 0.15, ring: 0.1, pinky: 0.1),

      // A relaxed hand making no gesture.
      GestureKind.none =>
        (thumb: 0.45, index: 0.5, middle: 0.5, ring: 0.45, pinky: 0.4),
    };

/// Builds the 21 landmarks of a hand.
///
/// [center] is the palm centre and [size] the palm width, both normalized.
/// [mirror] flips the hand across its own vertical axis, which is the
/// difference between a left and a right hand seen from the same side.
List<NormPoint> buildHandLandmarks({
  required NormPoint center,
  required double size,
  required GestureKind gesture,
  required bool mirror,
  double tilt = 0,
}) {
  final curl = curlFor(gesture);

  // Fingers point up the frame, so negative y. Lengths are fractions of palm
  // width, eyeballed from a hand.
  const fingers = [
    (base: -0.52, splay: -0.62, length: 0.78, lm: Lm.thumb), // thumb
    (base: -0.26, splay: -0.14, length: 1.02, lm: Lm.index),
    (base: -0.02, splay: 0.0, length: 1.10, lm: Lm.middle),
    (base: 0.20, splay: 0.12, length: 1.00, lm: Lm.ring),
    (base: 0.40, splay: 0.26, length: 0.82, lm: Lm.pinky),
  ];

  final points = List<NormPoint>.filled(kLandmarkCount, (x: 0.0, y: 0.0));

  NormPoint place(double dx, double dy) {
    final mx = mirror ? -dx : dx;
    final c = math.cos(tilt), s = math.sin(tilt);
    return (
      x: center.x + (mx * c - dy * s) * size,
      y: center.y + (mx * s + dy * c) * size,
    );
  }

  // The wrist sits below the palm centre.
  points[Lm.wrist] = place(0, 0.62);

  for (final f in fingers) {
    final isThumb = f.lm == Lm.thumb;
    final e = switch (f.lm) {
      Lm.thumb => curl.thumb,
      Lm.index => curl.index,
      Lm.middle => curl.middle,
      Lm.ring => curl.ring,
      _ => curl.pinky,
    };

    // Knuckle, on the arc across the top of the palm.
    var x = f.base;
    var y = isThumb ? 0.26 : -0.18;
    points[f.lm] = place(x, y);

    // Straight, the finger continues along its splay. Folded, each joint
    // bends further back toward the palm, which is what makes a fist read as
    // a fist rather than as short fingers.
    final straight = f.splay + (isThumb ? -0.5 : 0);
    final segment = f.length / 3;

    for (var j = 1; j <= 3; j++) {
      final bend = (1 - e) * j * 0.85;
      final angle = straight + bend * (isThumb ? -1.0 : 1.0) + bend * 0.15;

      // Angle 0 means straight up the frame.
      x += math.sin(angle) * segment;
      y -= math.cos(angle) * segment;
      points[f.lm + j] = place(x, y);
    }
  }

  return points;
}
