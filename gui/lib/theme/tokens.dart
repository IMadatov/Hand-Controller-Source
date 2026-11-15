import 'package:flutter/material.dart';

/// The design tokens, read off the mockups in `Design/`.
///
/// Every colour the UI uses is named here and nowhere else. A widget writing
/// `Color(0xFF7C6CF5)` inline is a token that cannot be changed in one place,
/// which is the whole reason this file exists.
abstract final class HcColors {
  // --- Surfaces, darkest to lightest ---------------------------------------

  /// The window behind everything, and the preview letterbox.
  static const canvas = Color(0xFF08090F);

  /// The default page background.
  static const background = Color(0xFF0F1119);

  /// Slightly raised: the sidebar, and panels sitting on `background`.
  static const surface = Color(0xFF161923);

  /// A card, a row, an inert button.
  static const surfaceRaised = Color(0xFF1A1D2A);

  /// A hovered or selected row.
  static const surfaceActive = Color(0xFF1E2130);

  // --- Lines ---------------------------------------------------------------

  /// Hairline between a card and its background.
  static const border = Color(0xFF252938);

  /// A border that needs to read as interactive.
  static const borderStrong = Color(0xFF3A4055);

  // --- Text ----------------------------------------------------------------

  /// Primary copy. Not pure white — the mockups never use #FFFFFF for text.
  static const text = Color(0xFFE2E5F0);

  /// Secondary copy: values beside a label, help text.
  static const textMuted = Color(0xFF8890A4);

  /// Labels above a value, timestamps, units. Deliberately low contrast;
  /// never use it for anything the user must read to act.
  static const textFaint = Color(0xFF4A5068);

  /// For text on a saturated fill.
  static const textOnAccent = Color(0xFFFAF9F5);

  // --- Accents -------------------------------------------------------------

  /// The brand accent: active nav, primary buttons, the command glyph.
  static const accent = Color(0xFF7C6CF5);

  /// A lighter accent for a hover or a gradient stop.
  static const accentBright = Color(0xFF9B8CF7);

  /// The LEFT hand. Paired with [handRight] below.
  static const handLeft = Color(0xFF4D8CF5);

  /// The RIGHT hand.
  ///
  /// Left and right get different hues on purpose: the acceptance criterion
  /// for phase 2 is an arrow key held with one hand while the other works the
  /// cursor, and a user cannot verify that unless the two hands are
  /// distinguishable at a glance.
  static const handRight = Color(0xFFF5844D);

  // --- Status --------------------------------------------------------------

  static const ok = Color(0xFF3ECFA8);
  static const warn = Color(0xFFF5B84D);
  static const danger = Color(0xFFF5614D);

  /// Tinted backgrounds for status pills. Dark enough to sit on [background]
  /// without the pill becoming the loudest thing on the screen.
  static const okBackground = Color(0xFF081F14);
  static const warnBackground = Color(0xFF352808);
  static const dangerBackground = Color(0xFF2A1215);

  /// Border for a danger-tinted control, e.g. the emergency-stop button.
  static const dangerBorder = Color(0xFF3D1515);
}

/// Corner radii. The mockups use exactly three.
abstract final class HcRadius {
  /// Pills, small badges, inline chips.
  static const sm = 4.0;

  /// Cards, buttons, inputs — the default.
  static const md = 8.0;

  /// Panels and the preview surface.
  static const lg = 12.0;
}

/// The spacing scale. Values are the ones the mockups actually use, not a
/// generated 4-point ramp, so an unusual number here is a deliberate choice
/// rather than a rounding.
abstract final class HcSpace {
  static const xs = 4.0;
  static const sm = 7.0;
  static const md = 10.0;
  static const lg = 14.0;
  static const xl = 20.0;
  static const xxl = 28.0;
}

/// Font families. Both are bundled; see `pubspec.yaml`.
abstract final class HcFont {
  /// UI copy.
  static const sans = 'Inter';

  /// Headings and the wordmark.
  static const display = 'Space Grotesk';

  /// Numbers that change every frame — fps, latency, confidence.
  ///
  /// Inter again, not a monospace family. What a live counter actually needs
  /// is TABULAR FIGURES — equal-width digits — so the row does not reflow as
  /// the number ticks, and Inter has them. Asking fontconfig for `monospace`
  /// would get a different face on every machine and nothing at all in a
  /// test, which is how a readout ends up rendering as empty boxes.
  ///
  /// Always pair this with `FontFeature.tabularFigures()`.
  static const mono = sans;
}
