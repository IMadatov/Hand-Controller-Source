import 'package:flutter_test/flutter_test.dart';
import 'package:hand_controller_gui/domain/bindings.dart';
import 'package:hand_controller_gui/domain/command.dart';
import 'package:hand_controller_gui/domain/gesture.dart';
import 'package:hand_controller_gui/engine/engine.dart';
import 'package:hand_controller_gui/engine/mock_engine.dart';
import 'package:hand_controller_gui/state/bindings_controller.dart';

Future<BindingsController> loaded() async {
  final controller = BindingsController(MockEngine());
  await controller.load();
  return controller;
}

Binding rowFor(BindingsController c, GestureKind gesture, HandSelector hand) =>
    c.draft!.bindings.firstWhere(
      (b) =>
          b.gesture == gesture &&
          b.hand == hand &&
          b.command.kind != CommandKind.keyRelease,
    );

void main() {
  test('loads a valid set and starts clean', () async {
    final c = await loaded();

    expect(c.draft, isNotNull);
    expect(c.draft!.isValid, isTrue);
    expect(c.isDirty, isFalse);
  });

  group('a hold always carries its release', () {
    test('rebinding to a key hold generates one', () async {
      final c = await loaded();
      final row = rowFor(c, GestureKind.openPalm, HandSelector.right);

      c.rebind(row, const HcCommand(kind: CommandKind.keyPress, key: HcKey.space));

      expect(c.draft!.unmatchedKeyPresses, isEmpty);
      expect(c.draft!.isValid, isTrue);
      expect(
        c.draft!.bindings.where((b) =>
            b.command.kind == CommandKind.keyRelease &&
            b.command.key == HcKey.space &&
            b.gesture == GestureKind.openPalm &&
            b.hand == HandSelector.right),
        hasLength(1),
      );
    });

    test('rebinding away from a hold removes the orphaned release', () async {
      final c = await loaded();
      final row = rowFor(c, GestureKind.thumbUp, HandSelector.left);
      expect(row.command.kind, CommandKind.keyPress);

      c.rebind(row, const HcCommand(kind: CommandKind.click));

      // The arrow_right release belonged to the hold that just went away.
      expect(
        c.draft!.bindings.where((b) =>
            b.gesture == GestureKind.thumbUp &&
            b.command.kind == CommandKind.keyRelease),
        isEmpty,
      );
      expect(c.draft!.isValid, isTrue);
    });

    test('changing the held key re-pairs rather than stacking', () async {
      final c = await loaded();
      final row = rowFor(c, GestureKind.thumbUp, HandSelector.left);

      c.rebind(row,
          const HcCommand(kind: CommandKind.keyPress, key: HcKey.escape));

      final releases = c.draft!.bindings.where((b) =>
          b.gesture == GestureKind.thumbUp &&
          b.command.kind == CommandKind.keyRelease);

      expect(releases, hasLength(1));
      expect(releases.single.command.key, HcKey.escape);
      expect(c.draft!.isValid, isTrue);
    });

    test('disabling a hold disables its release too', () async {
      final c = await loaded();
      final row = rowFor(c, GestureKind.pinkyOnly, HandSelector.left);

      c.setEnabled(row, false);

      final pair = c.draft!.bindings
          .where((b) => b.gesture == GestureKind.pinkyOnly &&
              b.hand == HandSelector.left);
      expect(pair, hasLength(2));
      expect(pair.every((b) => !b.enabled), isTrue);

      // An enabled press beside a disabled release is the stuck key.
      expect(c.draft!.isValid, isTrue);
    });

    test('removing a hold removes its release', () async {
      final c = await loaded();
      final row = rowFor(c, GestureKind.fourFingers, HandSelector.left);

      c.remove(row);

      expect(
        c.draft!.bindings.where((b) => b.gesture == GestureKind.fourFingers),
        isEmpty,
      );
    });
  });

  group('one command per slot', () {
    test('adding to a taken slot replaces it', () async {
      final c = await loaded();
      final before = c.draft!.bindings.length;

      c.add(const Binding(
        hand: HandSelector.right,
        gesture: GestureKind.openPalm,
        phase: GesturePhase.began,
        command: HcCommand(kind: CommandKind.click),
      ));

      final matching = c.draft!.bindings.where((b) =>
          b.hand == HandSelector.right && b.gesture == GestureKind.openPalm);

      expect(matching, hasLength(1));
      expect(matching.single.command.kind, CommandKind.click);
      expect(c.draft!.bindings.length, before);
    });
  });

  group('the phase follows the command', () {
    test('continuous commands fire while held', () {
      expect(phaseFor(CommandKind.moveCursor), GesturePhase.held);
      expect(phaseFor(CommandKind.scroll), GesturePhase.held);
    });

    test('one-shot commands fire on start', () {
      expect(phaseFor(CommandKind.click), GesturePhase.began);
      expect(phaseFor(CommandKind.doubleClick), GesturePhase.began);
      expect(phaseFor(CommandKind.keyPress), GesturePhase.began);
    });

    test('rebinding moves the row onto the right phase', () async {
      final c = await loaded();
      final row = rowFor(c, GestureKind.twoFingers, HandSelector.right);
      expect(row.phase, GesturePhase.began);

      c.rebind(row, const HcCommand(kind: CommandKind.scroll));

      // A click left on `held` would fire ~28 times a second.
      expect(
        rowFor(c, GestureKind.twoFingers, HandSelector.right).phase,
        GesturePhase.held,
      );
    });
  });

  group('saving', () {
    test('a clean set is not dirty, an edit is', () async {
      final c = await loaded();
      expect(c.isDirty, isFalse);

      c.setSensitivity(3.5);
      expect(c.isDirty, isTrue);

      c.revert();
      expect(c.isDirty, isFalse);
    });

    test('disabled rows are reported before they are dropped', () async {
      final c = await loaded();
      final row = rowFor(c, GestureKind.pinch, HandSelector.right);

      c.setEnabled(row, false);

      expect(c.disabledRows, isNotEmpty);
    });

    test('a saved draft round-trips back through the engine', () async {
      final engine = MockEngine();
      final c = BindingsController(engine);
      await c.load();

      c.setSensitivity(4.25);
      expect(await c.save(), isTrue);
      expect(c.isDirty, isFalse);

      final reread = await engine.loadBindings();
      expect(reread.sensitivity, 4.25);
    });

    test('the engine rejecting a set surfaces the reason', () async {
      final engine = MockEngine();
      final c = BindingsController(engine);
      await c.load();

      // Bypasses the editor's own guard to reach the engine's, which is the
      // backstop rather than the first line of defence.
      await expectLater(
        engine.saveBindings(BindingSet(bindings: [
          const Binding(
            hand: HandSelector.left,
            gesture: GestureKind.fist,
            phase: GesturePhase.began,
            command: HcCommand(kind: CommandKind.keyPress, key: HcKey.tab),
          ),
        ])),
        throwsA(isA<BindingsRejected>()),
      );
    });
  });
}
