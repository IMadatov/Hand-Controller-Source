import 'package:flutter/material.dart';

import 'tokens.dart';

/// The app's single [ThemeData].
///
/// Material defaults are overridden rather than worked around in widgets: if
/// a screen has to wrap something in a `Theme` to look right, the default
/// below is wrong and should be fixed here.
ThemeData buildHcTheme() {
  const scheme = ColorScheme.dark(
    primary: HcColors.accent,
    onPrimary: HcColors.textOnAccent,
    secondary: HcColors.handRight,
    onSecondary: HcColors.textOnAccent,
    surface: HcColors.surface,
    onSurface: HcColors.text,
    error: HcColors.danger,
    onError: HcColors.textOnAccent,
    outline: HcColors.border,
  );

  final base = ThemeData.from(colorScheme: scheme, useMaterial3: true);

  return base.copyWith(
    scaffoldBackgroundColor: HcColors.background,
    canvasColor: HcColors.background,
    dividerColor: HcColors.border,
    splashFactory: NoSplash.splashFactory,

    // The mockups show no ripples and no hover elevation. This is a desktop
    // app; a tap is a click, and a click does not need an ink spread.
    hoverColor: HcColors.surfaceActive,
    highlightColor: Colors.transparent,

    textTheme: _textTheme(base.textTheme),

    tooltipTheme: const TooltipThemeData(
      decoration: BoxDecoration(
        color: HcColors.surfaceRaised,
        border: Border.fromBorderSide(BorderSide(color: HcColors.border)),
        borderRadius: BorderRadius.all(Radius.circular(HcRadius.md)),
      ),
      textStyle: TextStyle(
        fontFamily: HcFont.sans,
        fontSize: 12,
        color: HcColors.text,
      ),
      waitDuration: Duration(milliseconds: 400),
    ),

    scrollbarTheme: ScrollbarThemeData(
      thumbColor: WidgetStateProperty.all(HcColors.borderStrong),
      thickness: WidgetStateProperty.all(6),
      radius: const Radius.circular(HcRadius.sm),
    ),

    sliderTheme: const SliderThemeData(
      activeTrackColor: HcColors.accent,
      inactiveTrackColor: HcColors.surfaceActive,
      thumbColor: HcColors.text,
      overlayColor: Colors.transparent,
      trackHeight: 4,
    ),

    switchTheme: SwitchThemeData(
      thumbColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.selected)
            ? HcColors.textOnAccent
            : HcColors.textFaint,
      ),
      trackColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.selected)
            ? HcColors.accent
            : HcColors.surfaceActive,
      ),
      trackOutlineColor: WidgetStateProperty.all(HcColors.border),
    ),
  );
}

TextTheme _textTheme(TextTheme base) {
  TextStyle sans(double size, FontWeight weight, Color color,
          {double? height}) =>
      TextStyle(
        fontFamily: HcFont.sans,
        fontSize: size,
        fontWeight: weight,
        color: color,
        height: height,
      );

  return base.copyWith(
    // Screen titles use the display face.
    headlineMedium: const TextStyle(
      fontFamily: HcFont.display,
      fontSize: 22,
      fontWeight: FontWeight.w600,
      color: HcColors.text,
    ),
    titleMedium: sans(15, FontWeight.w600, HcColors.text),
    titleSmall: sans(13, FontWeight.w600, HcColors.text),
    bodyMedium: sans(13, FontWeight.w400, HcColors.text, height: 1.45),
    bodySmall: sans(12, FontWeight.w400, HcColors.textMuted, height: 1.45),

    // The 10px faint label above a value. Used constantly in the mockups.
    labelSmall: sans(10, FontWeight.w500, HcColors.textFaint),
  );
}

/// A number that updates every frame: fps, latency, confidence.
///
/// Monospace and tabular so the row does not reflow as digits change.
const TextStyle hcMetricStyle = TextStyle(
  fontFamily: HcFont.mono,
  fontSize: 12,
  fontWeight: FontWeight.w600,
  color: HcColors.text,
  fontFeatures: [FontFeature.tabularFigures()],
);
