/// Mirrors `cpp/control/include/hc/command.hpp`.
///
/// NOTHING IN THIS FILE NAMES A PLATFORM API, and that is its purpose. A
/// command means the same thing on X11, on Windows, on macOS, and to a client
/// that cannot inject input at all and will only display it. The translation
/// to a keysym or a virtual-key code happens in the sink, in C++, never here.
library;

import 'package:flutter/foundation.dart';

/// Which mouse button a click refers to.
enum MouseButton {
  left('left', 'Left'),
  right('right', 'Right'),
  middle('middle', 'Middle');

  const MouseButton(this.wire, this.label);

  final String wire;
  final String label;

  static MouseButton? fromWire(String wire) {
    for (final b in values) {
      if (b.wire == wire) return b;
    }
    return null;
  }
}

/// An abstract key — deliberately not a keysym, not a virtual-key code and
/// not a scancode.
///
/// The list is short on purpose. Every entry has to be translated in EVERY
/// sink, so adding one here is a promise each future platform must keep. Do
/// not extend this list without the C++ side extending `hc::Key` first; a key
/// the core does not know is a configuration it will reject.
enum HcKey {
  arrowUp('arrow_up', 'Up arrow', '↑'),
  arrowDown('arrow_down', 'Down arrow', '↓'),
  arrowLeft('arrow_left', 'Left arrow', '←'),
  arrowRight('arrow_right', 'Right arrow', '→'),
  printScreen('print_screen', 'Print Screen', '⎙'),
  escape('escape', 'Escape', 'Esc'),
  enter('enter', 'Enter', '⏎'),
  space('space', 'Space', '␣'),
  tab('tab', 'Tab', '⇥');

  const HcKey(this.wire, this.label, this.glyph);

  final String wire;
  final String label;

  /// A short symbol for dense rows, where the full label will not fit.
  final String glyph;

  static HcKey? fromWire(String wire) {
    for (final k in values) {
      if (k.wire == wire) return k;
    }
    return null;
  }
}

/// What kind of action a command is.
enum CommandKind {
  /// A default-constructed command is inert, so a sink receiving one by
  /// mistake does nothing rather than clicking. The C++ parser rejects it as
  /// a binding target.
  none('none', 'Nothing'),

  moveCursor('move_cursor', 'Move cursor'),
  click('click', 'Click'),
  doubleClick('double_click', 'Double click'),
  scroll('scroll', 'Scroll'),
  keyPress('key_press', 'Hold key'),
  keyRelease('key_release', 'Release key');

  const CommandKind(this.wire, this.label);

  final String wire;
  final String label;

  /// True when this kind requires a key argument.
  bool get needsKey => this == keyPress || this == keyRelease;

  /// True when this kind takes an optional button argument.
  bool get takesButton => this == click || this == doubleClick;

  /// The kinds offered in the bindings editor.
  ///
  /// [none] is excluded because it is not a thing a user wants; disabling a
  /// row is how you express "do nothing", and that is a separate control.
  ///
  /// [keyRelease] is excluded because the user never authors one directly —
  /// it is generated as the partner of a [keyPress]. See `BindingSet`.
  static List<CommandKind> get authorable => values
      .where((c) => c != CommandKind.none && c != CommandKind.keyRelease)
      .toList(growable: false);

  static CommandKind? fromWire(String wire) {
    for (final c in values) {
      if (c.wire == wire) return c;
    }
    return null;
  }
}

/// One abstract action.
///
/// Which fields carry meaning depends on [kind]; the rest stay at their
/// defaults rather than being set to something plausible.
///
/// [delta] IS RELATIVE AND NORMALIZED, never an absolute screen position.
/// Exactly one layer multiplies it by pixels, and that layer is the sink.
/// Relative motion is also what lets a small hand movement cross a wide
/// display, which the Python version's absolute 1:1 mapping could not do.
@immutable
class HcCommand {
  const HcCommand({
    required this.kind,
    this.delta = (x: 0.0, y: 0.0),
    this.button = MouseButton.left,
    this.key,
  });

  final CommandKind kind;
  final ({double x, double y}) delta;
  final MouseButton button;

  /// Null unless [kind] is a key command. Null here with a key kind is an
  /// invalid command, and the C++ parser says so.
  final HcKey? key;

  /// How this command reads in a log line or a binding row.
  String get label => switch (kind) {
        CommandKind.none => 'Nothing',
        CommandKind.moveCursor => 'Move cursor',
        CommandKind.scroll => 'Scroll',
        CommandKind.click => '${button.label} click',
        CommandKind.doubleClick => 'Double click',
        CommandKind.keyPress => 'Hold ${key?.label ?? '—'}',
        CommandKind.keyRelease => 'Release ${key?.label ?? '—'}',
      };

  /// The argument as it appears after the command in the bindings file, or
  /// null when the command takes none.
  String? get wireArgument => switch (kind) {
        CommandKind.click || CommandKind.doubleClick => button.wire,
        CommandKind.keyPress || CommandKind.keyRelease => key?.wire,
        _ => null,
      };

  HcCommand copyWith({
    CommandKind? kind,
    ({double x, double y})? delta,
    MouseButton? button,
    HcKey? key,
  }) =>
      HcCommand(
        kind: kind ?? this.kind,
        delta: delta ?? this.delta,
        button: button ?? this.button,
        key: key ?? this.key,
      );

  @override
  bool operator ==(Object other) =>
      other is HcCommand &&
      other.kind == kind &&
      other.button == button &&
      other.key == key &&
      other.delta == delta;

  @override
  int get hashCode => Object.hash(kind, button, key, delta);
}
