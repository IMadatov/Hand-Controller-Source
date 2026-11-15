/// Mirrors `cpp/control/include/hc/bindings.hpp`.
///
/// This file models the bindings; it does NOT parse or serialize the bindings
/// file. `ParseBindings` and `SerializeBindings` live in C++ and are reached
/// through the engine, because a second implementation of the format here
/// would drift from the first one. See `docs/gui-contract.md`, section 5.
library;

import 'package:flutter/foundation.dart';

import 'command.dart';
import 'gesture.dart';

/// Which hand a binding applies to.
///
/// This is NOT [Handedness], and the difference is the point. Handedness has
/// a third value, `unknown`, meaning "the model could not tell" — a
/// description of a hand, not something a user would ever write in a
/// configuration. With only left and right available to bind, an unlabelled
/// hand would match nothing and every gesture it made would silently do
/// nothing.
///
/// [any] is the fix, and it is why this type exists separately.
enum HandSelector {
  /// Matches left, right AND unlabelled hands alike.
  any('any', 'Either hand'),

  left('left', 'Left hand'),
  right('right', 'Right hand');

  const HandSelector(this.wire, this.label);

  final String wire;
  final String label;

  /// True when a gesture from a hand labelled [handedness] is matched by a
  /// binding written for this selector.
  bool matches(Handedness handedness) => switch (this) {
        HandSelector.any => true,
        HandSelector.left => handedness == Handedness.left,
        HandSelector.right => handedness == Handedness.right,
      };

  static HandSelector? fromWire(String wire) {
    for (final h in values) {
      if (h.wire == wire) return h;
    }
    return null;
  }
}

/// One line of the configuration: a gesture, in a phase, on a hand, does this.
@immutable
class Binding {
  const Binding({
    required this.hand,
    required this.gesture,
    required this.phase,
    required this.command,
    this.enabled = true,
  });

  final HandSelector hand;
  final GestureKind gesture;
  final GesturePhase phase;
  final HcCommand command;

  /// A UI-only flag with no counterpart in the file format.
  ///
  /// A disabled binding is simply ABSENT from what gets written, because the
  /// format has no way to say "present but off". The consequence is real and
  /// deliberate: disabling a row and saving loses the row. Until the format
  /// grows a representation for it, the editor must warn before a save that
  /// would drop rows rather than let them vanish quietly.
  final bool enabled;

  /// The identity of a binding for the purpose of editing: one command per
  /// (hand, gesture, phase). Two commands on the same triple would make the
  /// order of the file decide the behaviour, which is not something a user
  /// can see or control.
  ({HandSelector hand, GestureKind gesture, GesturePhase phase}) get slot =>
      (hand: hand, gesture: gesture, phase: phase);

  Binding copyWith({
    HandSelector? hand,
    GestureKind? gesture,
    GesturePhase? phase,
    HcCommand? command,
    bool? enabled,
  }) =>
      Binding(
        hand: hand ?? this.hand,
        gesture: gesture ?? this.gesture,
        phase: phase ?? this.phase,
        command: command ?? this.command,
        enabled: enabled ?? this.enabled,
      );

  @override
  bool operator ==(Object other) =>
      other is Binding &&
      other.hand == hand &&
      other.gesture == gesture &&
      other.phase == phase &&
      other.command == command &&
      other.enabled == enabled;

  @override
  int get hashCode => Object.hash(hand, gesture, phase, command, enabled);
}

/// Defaults applied when a configuration does not say otherwise.
///
/// BOTH ARE CHOSEN, NOT MEASURED. There was no camera on the machine these
/// were written on, so no value here came from watching a cursor move. They
/// are starting points to tune, and the UI should present them that way
/// rather than as correct values.
const double kDefaultSensitivity = 2.0;
const double kDefaultSmoothing = 0.5;

/// A problem that makes a configuration invalid.
///
/// The C++ parser rejects these too. The editor surfaces them first so the
/// user is not told "rejected" after pressing Save.
@immutable
class BindingProblem {
  const BindingProblem({required this.message, this.binding});

  final String message;

  /// The row at fault, or null when the problem is about the set as a whole.
  final Binding? binding;
}

/// A whole configuration: the bindings plus the two tunables.
@immutable
class BindingSet {
  const BindingSet({
    required this.bindings,
    this.sensitivity = kDefaultSensitivity,
    this.smoothing = kDefaultSmoothing,
  });

  final List<Binding> bindings;

  /// Multiplies normalized cursor motion. Must be strictly positive. Above 1
  /// so a small hand movement can cross a wide display.
  final double sensitivity;

  /// Exponential smoothing applied by the mapper, in `[0, 1]`. 0 is none; 1
  /// would be a cursor that never moves.
  final double smoothing;

  /// Everything wrong with this set, in the order a user should fix it.
  ///
  /// The first rule is the important one, and it is the reason this method
  /// exists rather than leaving validation to the C++ parser:
  ///
  /// > every `key_press` needs a matching `key_release` on the `ended` phase.
  ///
  /// A key still held after the gesture stops is the worst failure this
  /// project can produce — a stuck modifier is worse than a missed click —
  /// so the editor refuses to author one rather than reporting it afterwards.
  List<BindingProblem> get problems {
    final found = <BindingProblem>[];

    for (final b in bindings.where((b) => b.enabled)) {
      if (b.command.kind == CommandKind.none) {
        found.add(BindingProblem(
          message: '${b.gesture.label}: no command chosen.',
          binding: b,
        ));
      }
      if (b.command.kind.needsKey && b.command.key == null) {
        found.add(BindingProblem(
          message: '${b.gesture.label}: a key command needs a key.',
          binding: b,
        ));
      }
    }

    for (final press in unmatchedKeyPresses) {
      found.add(BindingProblem(
        message: '${press.gesture.label} holds '
            '${press.command.key?.label ?? 'a key'} but never releases it. '
            'The key would stay down after the gesture stops.',
        binding: press,
      ));
    }

    if (!(sensitivity > 0)) {
      found.add(const BindingProblem(
        message: 'Cursor sensitivity must be greater than zero.',
      ));
    }
    if (smoothing < 0 || smoothing > 1) {
      found.add(const BindingProblem(
        message: 'Smoothing must be between 0 and 1.',
      ));
    }

    return found;
  }

  /// Every enabled `key_press` with no `key_release` partner.
  ///
  /// A partner is a `key_release` of the SAME key, on the same hand and
  /// gesture, in the [GesturePhase.ended] phase. Releasing a different key,
  /// or releasing on a different gesture, does not count — the key would
  /// still be held when this gesture ended.
  List<Binding> get unmatchedKeyPresses => bindings
      .where((b) =>
          b.enabled &&
          b.command.kind == CommandKind.keyPress &&
          b.command.key != null &&
          !_hasRelease(b))
      .toList(growable: false);

  bool _hasRelease(Binding press) => bindings.any((other) =>
      other.enabled &&
      other.command.kind == CommandKind.keyRelease &&
      other.command.key == press.command.key &&
      other.hand == press.hand &&
      other.gesture == press.gesture &&
      other.phase == GesturePhase.ended);

  bool get isValid => problems.isEmpty;

  /// The release that must accompany [press] for the set to stay valid.
  ///
  /// The editor adds this automatically when a user authors a key hold, which
  /// is why `CommandKind.authorable` does not offer `key_release`: pairing is
  /// the editor's job, not a thing to get right by hand.
  static Binding releaseFor(Binding press) => Binding(
        hand: press.hand,
        gesture: press.gesture,
        phase: GesturePhase.ended,
        command: HcCommand(
          kind: CommandKind.keyRelease,
          key: press.command.key,
        ),
        enabled: press.enabled,
      );

  BindingSet copyWith({
    List<Binding>? bindings,
    double? sensitivity,
    double? smoothing,
  }) =>
      BindingSet(
        bindings: bindings ?? this.bindings,
        sensitivity: sensitivity ?? this.sensitivity,
        smoothing: smoothing ?? this.smoothing,
      );
}
