import 'package:flutter/material.dart';

import '../domain/gesture.dart';
import '../domain/observation.dart';
import '../theme/tokens.dart';
import 'common.dart';

/// Where to draw a hand's centre.
///
/// COMPUTED, NOT REPORTED. `HandObservation` carries landmarks and nothing
/// derived; the recognizer's own palm centre reaches the UI on a
/// `GestureEvent`, and only when a gesture is active. A hand doing nothing
/// still has to be drawn, so the painter averages the wrist and the four
/// finger knuckles — a drawing concern, kept in the painter.
NormPoint palmCentreOf(HandObservation hand) {
  var x = 0.0, y = 0.0;
  for (final i in Lm.palmOutline) {
    x += hand.landmarks[i].x;
    y += hand.landmarks[i].y;
  }
  final n = Lm.palmOutline.length;
  return (x: x / n, y: y / n);
}

/// Draws the tracked hands over the camera image.
///
/// This is one of exactly two places in the whole project permitted to
/// multiply a normalized coordinate by a pixel dimension — the other is the
/// output sink. Everything upstream speaks `0..1`.
class HandOverlayPainter extends CustomPainter {
  const HandOverlayPainter({required this.hands, required this.gestures});

  final List<HandObservation> hands;

  /// The pose each track currently holds, by track id.
  ///
  /// Comes from the engine's gesture EVENTS, not from the observations —
  /// the observations do not know what a gesture is. A track missing from
  /// this map is holding nothing, which is a normal state.
  final Map<int, GestureKind> gestures;

  @override
  void paint(Canvas canvas, Size size) {
    for (final hand in hands) {
      final color = handColor(hand.handedness);
      Offset at(NormPoint p) => Offset(p.x * size.width, p.y * size.height);

      // Bones first, so the joints sit on top of them.
      final bone = Paint()
        ..color = color.withValues(alpha: 0.75)
        ..strokeWidth = 2
        ..strokeCap = StrokeCap.round;

      for (final (a, b) in Lm.connections) {
        canvas.drawLine(at(hand.landmarks[a]), at(hand.landmarks[b]), bone);
      }

      // Joints. Fingertips are drawn larger because they are what the
      // recognizer actually measures — pinch distance is tip to tip.
      const tips = {4, 8, 12, 16, 20};
      for (var i = 0; i < hand.landmarks.length; i++) {
        final isTip = tips.contains(i);
        canvas.drawCircle(
          at(hand.landmarks[i]),
          isTip ? 3.6 : 2.2,
          Paint()..color = isTip ? color : color.withValues(alpha: 0.85),
        );
      }

      final centre = palmCentreOf(hand);
      canvas.drawCircle(
        at(centre),
        4,
        Paint()
          ..color = color
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1.5,
      );

      _paintLabel(canvas, size, hand, color, at(centre));
    }
  }

  /// The gesture name, pinned above the hand.
  void _paintLabel(
    Canvas canvas,
    Size size,
    HandObservation hand,
    Color color,
    Offset center,
  ) {
    final gesture = gestures[hand.trackId];
    if (gesture == null || gesture == GestureKind.none) return;

    final text = TextPainter(
      text: TextSpan(
        text: gesture.label,
        style: TextStyle(
          fontFamily: HcFont.sans,
          fontSize: 11,
          fontWeight: FontWeight.w600,
          color: color,
        ),
      ),
      textDirection: TextDirection.ltr,
    )..layout();

    final padding = const Offset(6, 3);
    var origin = center - Offset(text.width / 2, 0.16 * size.height);

    // Keep the label inside the preview; a gesture name sliding off the edge
    // is exactly when the user most wants to read it.
    final maxX = size.width - text.width - padding.dx * 2;
    origin = Offset(
      origin.dx.clamp(padding.dx, maxX < padding.dx ? padding.dx : maxX),
      origin.dy.clamp(padding.dy + 10, size.height - 20),
    );

    final box = Rect.fromLTWH(
      origin.dx - padding.dx,
      origin.dy - padding.dy,
      text.width + padding.dx * 2,
      text.height + padding.dy * 2,
    );

    canvas.drawRRect(
      RRect.fromRectAndRadius(box, const Radius.circular(HcRadius.sm)),
      Paint()..color = HcColors.canvas.withValues(alpha: 0.82),
    );
    text.paint(canvas, origin);
  }

  @override
  bool shouldRepaint(HandOverlayPainter oldDelegate) =>
      !identical(oldDelegate.hands, hands) ||
      !identical(oldDelegate.gestures, gestures);
}

/// The camera image with the overlay on top.
///
/// There is no camera feed yet — the engine returns landmarks, not pixels —
/// so the surface renders as an honest empty frame rather than a fake video
/// still. When frames arrive they go behind the same overlay.
class PreviewSurface extends StatelessWidget {
  const PreviewSurface({
    super.key,
    required this.hands,
    required this.gestures,
    required this.caption,
    this.badge,
  });

  final List<HandObservation> hands;

  /// The pose each track holds, from the engine's gesture events.
  final Map<int, GestureKind> gestures;

  /// e.g. `1280x720 - HD`.
  final String caption;

  final Widget? badge;

  @override
  Widget build(BuildContext context) => ClipRRect(
        borderRadius: BorderRadius.circular(HcRadius.lg),
        child: DecoratedBox(
          decoration: BoxDecoration(
            color: HcColors.canvas,
            border: Border.all(color: HcColors.border),
            borderRadius: BorderRadius.circular(HcRadius.lg),
          ),
          child: AspectRatio(
            aspectRatio: 16 / 9,
            child: Stack(
              fit: StackFit.expand,
              children: [
                const _EmptyFrameHint(),
                CustomPaint(
                  painter: HandOverlayPainter(
                    hands: hands,
                    gestures: gestures,
                  ),
                ),
                if (badge != null)
                  Positioned(top: HcSpace.md, left: HcSpace.md, child: badge!),
                Positioned(
                  bottom: HcSpace.md,
                  right: HcSpace.md,
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: HcColors.canvas.withValues(alpha: 0.75),
                      borderRadius: BorderRadius.circular(HcRadius.sm),
                    ),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 6, vertical: 3),
                      child: Text(
                        caption,
                        style: const TextStyle(
                          fontFamily: HcFont.mono,
                          fontSize: 10,
                          color: HcColors.textMuted,
                        ),
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      );
}

/// A faint grid, so the preview reads as a camera surface with nothing in it
/// rather than as a failed image load.
class _EmptyFrameHint extends StatelessWidget {
  const _EmptyFrameHint();

  @override
  Widget build(BuildContext context) =>
      CustomPaint(painter: _GridPainter());
}

class _GridPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final line = Paint()
      ..color = HcColors.border.withValues(alpha: 0.35)
      ..strokeWidth = 1;

    for (var i = 1; i < 3; i++) {
      final x = size.width * i / 3;
      final y = size.height * i / 3;
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), line);
      canvas.drawLine(Offset(0, y), Offset(size.width, y), line);
    }
  }

  @override
  bool shouldRepaint(_GridPainter oldDelegate) => false;
}
