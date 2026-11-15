import 'package:flutter/foundation.dart';

/// Preferences that belong to this app rather than to the engine.
///
/// Deliberately separate from `BindingSet`: cursor sensitivity and smoothing
/// are part of the configuration the engine reads, while the language and
/// the panic hotkey are not. Keeping them in one bag would mean the bindings
/// file grew fields the C++ parser has no business knowing.
///
/// NOTHING HERE IS PERSISTED YET. The values live for the session only; a
/// store belongs behind this class once there is a real one to write to.
class AppSettings extends ChangeNotifier {
  /// Frame size requested from the camera.
  ///
  /// Not a cosmetic choice: resolution is the first thing to lower when the
  /// frame rate is too low, which is what the Diagnostics screen will say.
  CaptureResolution resolution = CaptureResolution.hd;

  /// How eager the detector is.
  DetectionThreshold threshold = DetectionThreshold.medium;

  /// Start tracking when the user logs in.
  bool launchAtStartup = false;

  /// Show raw numbers beside the controls that have them.
  bool showNumericValues = false;

  /// The key chord that stops everything.
  ///
  /// This exists because gesture input can go wrong in a way the user cannot
  /// click their way out of — a stuck key, or a cursor being driven away
  /// from the stop button. It must be reachable from the keyboard alone.
  String panicHotkey = 'Ctrl + Shift + S';

  UiLanguage language = UiLanguage.uzbek;

  void update(void Function() change) {
    change();
    notifyListeners();
  }
}

enum CaptureResolution {
  low('640 x 480', 'Low CPU'),
  hd('1280 x 720', 'Recommended'),
  fullHd('1920 x 1080', 'High CPU');

  const CaptureResolution(this.label, this.note);

  final String label;
  final String note;
}

enum DetectionThreshold {
  low('Low', 'Triggers easily. More false positives.'),
  medium('Medium', 'The default.'),
  high('High', 'Needs a clear pose. Fewer accidental commands.');

  const DetectionThreshold(this.label, this.note);

  final String label;
  final String note;
}

enum UiLanguage {
  uzbek("O'zbekcha", 'UZ'),
  english('English', 'EN'),
  russian('Русский', 'RU');

  const UiLanguage(this.label, this.code);

  final String label;
  final String code;
}
