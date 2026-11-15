import 'package:flutter/material.dart';

import '../domain/gesture.dart';
import '../theme/tokens.dart';

/// A bordered surface. The mockups' only container.
class HcPanel extends StatelessWidget {
  const HcPanel({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(HcSpace.lg),
    this.color = HcColors.surfaceRaised,
    this.radius = HcRadius.lg,
    this.borderColor = HcColors.border,
  });

  final Widget child;
  final EdgeInsetsGeometry padding;
  final Color color;
  final double radius;
  final Color borderColor;

  @override
  Widget build(BuildContext context) => DecoratedBox(
        decoration: BoxDecoration(
          color: color,
          borderRadius: BorderRadius.circular(radius),
          border: Border.all(color: borderColor),
        ),
        child: Padding(padding: padding, child: child),
      );
}

/// A small tinted label: `Active`, `24 fps`, `CRITICAL`.
class HcPill extends StatelessWidget {
  const HcPill({
    super.key,
    required this.label,
    required this.color,
    this.background,
    this.icon,
  });

  final String label;
  final Color color;
  final Color? background;
  final IconData? icon;

  @override
  Widget build(BuildContext context) => DecoratedBox(
        decoration: BoxDecoration(
          color: background ?? color.withValues(alpha: 0.12),
          borderRadius: BorderRadius.circular(HcRadius.sm),
        ),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              if (icon != null) ...[
                Icon(icon, size: 11, color: color),
                const SizedBox(width: 4),
              ],
              Text(
                label,
                style: TextStyle(
                  fontFamily: HcFont.sans,
                  fontSize: 10,
                  fontWeight: FontWeight.w600,
                  color: color,
                ),
              ),
            ],
          ),
        ),
      );
}

/// A faint 10px label above a value. The mockups use this constantly.
class HcLabel extends StatelessWidget {
  const HcLabel(this.text, {super.key});

  final String text;

  @override
  Widget build(BuildContext context) => Text(
        text,
        style: Theme.of(context).textTheme.labelSmall,
      );
}

/// The colour that identifies a hand.
///
/// An unlabelled hand gets neither hue, and must not be coloured as a left or
/// a right one: the point of showing it is that the model could not tell.
Color handColor(Handedness handedness) => switch (handedness) {
      Handedness.left => HcColors.handLeft,
      Handedness.right => HcColors.handRight,
      Handedness.unknown => HcColors.textMuted,
    };

/// The square `L` / `R` badge from the mockups.
class HandBadge extends StatelessWidget {
  const HandBadge(this.handedness, {super.key, this.size = 22});

  final Handedness handedness;
  final double size;

  @override
  Widget build(BuildContext context) {
    final color = handColor(handedness);
    final glyph = switch (handedness) {
      Handedness.left => 'L',
      Handedness.right => 'R',
      Handedness.unknown => '?',
    };

    return Tooltip(
      message: handedness == Handedness.unknown
          ? 'The model could not tell which hand this is'
          : '${handedness.label} hand',
      child: Container(
        width: size,
        height: size,
        alignment: Alignment.center,
        decoration: BoxDecoration(
          color: color.withValues(alpha: 0.14),
          borderRadius: BorderRadius.circular(HcRadius.sm + 1),
          border: Border.all(color: color.withValues(alpha: 0.45)),
        ),
        child: Text(
          glyph,
          style: TextStyle(
            fontFamily: HcFont.display,
            fontSize: size * 0.5,
            fontWeight: FontWeight.w700,
            color: color,
          ),
        ),
      ),
    );
  }
}

/// A label above a live number, as used in the hand cards.
class HcStat extends StatelessWidget {
  const HcStat({
    super.key,
    required this.label,
    required this.value,
    this.valueColor,
    this.mono = false,
  });

  final String label;
  final String value;
  final Color? valueColor;

  /// True for numbers that tick every frame, so the row does not reflow.
  final bool mono;

  @override
  Widget build(BuildContext context) => Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          HcLabel(label),
          const SizedBox(height: 2),
          Text(
            value,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: mono
                ? const TextStyle(
                    fontFamily: HcFont.mono,
                    fontSize: 12,
                    fontWeight: FontWeight.w600,
                    color: HcColors.text,
                    fontFeatures: [FontFeature.tabularFigures()],
                  ).copyWith(color: valueColor)
                : TextStyle(
                    fontFamily: HcFont.sans,
                    fontSize: 13,
                    fontWeight: FontWeight.w500,
                    color: valueColor ?? HcColors.text,
                  ),
          ),
        ],
      );
}

/// A confidence bar, `0..1`.
///
/// Shown rather than hidden because a weak score explains behaviour the user
/// would otherwise read as a bug — a gesture that will not trigger, or the
/// wrong hand's binding firing.
class HcConfidence extends StatelessWidget {
  const HcConfidence({
    super.key,
    required this.value,
    required this.color,
    this.width = 54,
  });

  final double value;
  final Color color;
  final double width;

  @override
  Widget build(BuildContext context) {
    final clamped = value.clamp(0.0, 1.0);
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Container(
          width: width,
          height: 4,
          decoration: BoxDecoration(
            color: HcColors.surfaceActive,
            borderRadius: BorderRadius.circular(2),
          ),
          child: FractionallySizedBox(
            alignment: Alignment.centerLeft,
            widthFactor: clamped,
            child: DecoratedBox(
              decoration: BoxDecoration(
                color: color,
                borderRadius: BorderRadius.circular(2),
              ),
            ),
          ),
        ),
        const SizedBox(width: 6),
        Text(
          '${(clamped * 100).round()}%',
          style: TextStyle(
            fontFamily: HcFont.mono,
            fontSize: 10,
            fontWeight: FontWeight.w600,
            color: color,
            fontFeatures: const [FontFeature.tabularFigures()],
          ),
        ),
      ],
    );
  }
}

/// A flat button in the mockups' style.
class HcButton extends StatelessWidget {
  const HcButton({
    super.key,
    required this.label,
    this.icon,
    this.onPressed,
    this.primary = false,
    this.danger = false,
    this.tooltip,
  });

  final String label;
  final IconData? icon;
  final VoidCallback? onPressed;
  final bool primary;
  final bool danger;
  final String? tooltip;

  @override
  Widget build(BuildContext context) {
    final enabled = onPressed != null;
    final (bg, fg, border) = switch ((primary, danger)) {
      (_, true) => (Colors.transparent, HcColors.danger, HcColors.dangerBorder),
      (true, _) => (HcColors.accent, HcColors.textOnAccent, HcColors.accent),
      _ => (HcColors.surfaceRaised, HcColors.text, HcColors.border),
    };

    final button = Opacity(
      opacity: enabled ? 1 : 0.4,
      child: Material(
        color: bg,
        borderRadius: BorderRadius.circular(HcRadius.md),
        child: InkWell(
          onTap: onPressed,
          borderRadius: BorderRadius.circular(HcRadius.md),
          child: Container(
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(HcRadius.md),
              border: Border.all(color: border),
            ),
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                if (icon != null) ...[
                  Icon(icon, size: 13, color: fg),
                  if (label.isNotEmpty) const SizedBox(width: 6),
                ],
                if (label.isNotEmpty)
                  Text(
                    label,
                    style: TextStyle(
                      fontFamily: HcFont.sans,
                      fontSize: 12,
                      fontWeight: FontWeight.w600,
                      color: fg,
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );

    return tooltip == null ? button : Tooltip(message: tooltip!, child: button);
  }
}
