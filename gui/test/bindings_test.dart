import 'package:flutter_test/flutter_test.dart';
import 'package:hand_controller_gui/domain/bindings.dart';
import 'package:hand_controller_gui/domain/command.dart';
import 'package:hand_controller_gui/domain/gesture.dart';
import 'package:hand_controller_gui/engine/default_bindings.dart';

Binding press(HcKey key, {GestureKind gesture = GestureKind.thumbUp}) =>
    Binding(
      hand: HandSelector.left,
      gesture: gesture,
      phase: GesturePhase.began,
      command: HcCommand(kind: CommandKind.keyPress, key: key),
    );

void main() {
  group('a key hold must be released', () {
    test('a press with no release is a problem', () {
      final set = BindingSet(bindings: [press(HcKey.arrowRight)]);

      expect(set.unmatchedKeyPresses, hasLength(1));
      expect(set.isValid, isFalse);
      expect(set.problems.single.message, contains('never releases'));
    });

    test('the generated partner satisfies it', () {
      final p = press(HcKey.arrowRight);
      final set = BindingSet(bindings: [p, BindingSet.releaseFor(p)]);

      expect(set.unmatchedKeyPresses, isEmpty);
      expect(set.isValid, isTrue);
    });

    test('releasing a different key does not count', () {
      final p = press(HcKey.arrowRight);
      final wrong = BindingSet.releaseFor(press(HcKey.arrowLeft));
      final set = BindingSet(bindings: [p, wrong]);

      // The key actually held is still down when the gesture ends.
      expect(set.unmatchedKeyPresses, hasLength(1));
    });

    test('releasing on a different gesture does not count', () {
      final p = press(HcKey.arrowUp, gesture: GestureKind.pointIndex);
      final elsewhere =
          BindingSet.releaseFor(press(HcKey.arrowUp, gesture: GestureKind.fist));
      final set = BindingSet(bindings: [p, elsewhere]);

      expect(set.unmatchedKeyPresses, hasLength(1));
    });

    test('a disabled release leaves the press unmatched', () {
      // The row is still in the list, so a check that only looked for its
      // presence would pass while the saved file would not contain it.
      final p = press(HcKey.arrowRight);
      final off = BindingSet.releaseFor(p).copyWith(enabled: false);
      final set = BindingSet(bindings: [p, off]);

      expect(set.unmatchedKeyPresses, hasLength(1));
    });

    test('a disabled press needs no release', () {
      final set = BindingSet(
        bindings: [press(HcKey.arrowRight).copyWith(enabled: false)],
      );

      expect(set.isValid, isTrue);
    });
  });

  group('the tunables have ranges', () {
    test('sensitivity must be positive', () {
      expect(const BindingSet(bindings: [], sensitivity: 0).isValid, isFalse);
      expect(const BindingSet(bindings: [], sensitivity: -1).isValid, isFalse);
    });

    test('smoothing must be a fraction', () {
      expect(const BindingSet(bindings: [], smoothing: 1.5).isValid, isFalse);
      expect(const BindingSet(bindings: [], smoothing: -0.1).isValid, isFalse);
      expect(const BindingSet(bindings: [], smoothing: 1).isValid, isTrue);
    });
  });

  group('a binding needs a command', () {
    test('none is rejected', () {
      const set = BindingSet(bindings: [
        Binding(
          hand: HandSelector.right,
          gesture: GestureKind.fist,
          phase: GesturePhase.began,
          command: HcCommand(kind: CommandKind.none),
        ),
      ]);

      expect(set.problems.map((p) => p.message).join(), contains('no command'));
    });

    test('a key command with no key is rejected', () {
      const set = BindingSet(bindings: [
        Binding(
          hand: HandSelector.left,
          gesture: GestureKind.fist,
          phase: GesturePhase.began,
          command: HcCommand(kind: CommandKind.keyPress),
        ),
      ]);

      expect(set.problems.map((p) => p.message).join(), contains('needs a key'));
    });
  });

  group('hand selectors', () {
    test('any matches an unlabelled hand', () {
      // The reason HandSelector exists. With only left and right to bind, a
      // hand the model could not label would match nothing.
      expect(HandSelector.any.matches(Handedness.unknown), isTrue);
      expect(HandSelector.left.matches(Handedness.unknown), isFalse);
      expect(HandSelector.right.matches(Handedness.unknown), isFalse);
    });
  });

  group('the shipped defaults', () {
    test('are valid', () {
      expect(mockDefaultBindings().isValid, isTrue);
    });

    test('release every key they hold', () {
      // Five left-hand holds in the default set, each needing a partner.
      final set = mockDefaultBindings();
      final presses = set.bindings
          .where((b) => b.command.kind == CommandKind.keyPress)
          .length;

      expect(presses, 5);
      expect(set.unmatchedKeyPresses, isEmpty);
    });

    test('wire names match the configuration file', () {
      // These strings appear in cpp/config/default_bindings.txt. If a rename
      // ever reaches this far, every saved configuration is already broken.
      expect(GestureKind.pointIndex.wire, 'point_index');
      expect(GestureKind.fourFingers.wire, 'four_fingers');
      expect(CommandKind.moveCursor.wire, 'move_cursor');
      expect(CommandKind.doubleClick.wire, 'double_click');
      expect(HcKey.printScreen.wire, 'print_screen');
      expect(GesturePhase.began.wire, 'began');
      expect(HandSelector.any.wire, 'any');
    });
  });
}
