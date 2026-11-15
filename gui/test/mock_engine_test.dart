/// Checks that the mock produces the SHAPE of event stream the real engine
/// produces, as pinned by
/// `docs/superpowers/specs/2026-11-15-gui-contract.md`.
///
/// The point of these is not that the mock is correct — it invents its data.
/// It is that the UI is never written against a transition the engine does
/// not actually make, which is the one failure a mock exists to cause.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:hand_controller_gui/domain/command.dart';
import 'package:hand_controller_gui/domain/gesture.dart';
import 'package:hand_controller_gui/domain/observation.dart';
import 'package:hand_controller_gui/engine/engine.dart';
import 'package:hand_controller_gui/engine/mock_engine.dart';

void main() {
  late MockEngine engine;
  late List<CommandRecord> commands;
  late List<EngineFrame> frames;

  // Gesture events are not on the interface — the UI consumes commands and
  // frames — so every assertion below is made where the UI can see it.
  setUp(() {
    commands = [];
    frames = [];
  });

  tearDown(() async => engine.dispose());

  Future<void> run({int forFrames = 600}) async {
    var tick = 0;
    engine = MockEngine(
      frameInterval: const Duration(milliseconds: 35),
      clock: () => DateTime.utc(2026).add(Duration(milliseconds: 35 * tick++)),
    );
    engine.commands.listen(commands.add);
    engine.frames.listen(frames.add);
    await engine.start();
    await Future<void>.delayed(Duration(milliseconds: 35 * forFrames));
  }

  test('a key hold is always followed by its release', () async {
    await run();

    // Stopping must release whatever is still held. A gesture in progress
    // when the engine shuts down would otherwise leave a key down with no
    // process left to lift it.
    await engine.stop();

    // Per track and key, presses and releases must alternate and end
    // balanced. An unbalanced tail is a key left down.
    final held = <(int, HcKey)>{};
    for (final c in commands) {
      final key = c.command.key;
      if (key == null) continue;
      final id = (c.handedness.index, key);

      if (c.command.kind == CommandKind.keyPress) {
        expect(held.contains(id), isFalse,
            reason: 'pressed ${key.wire} twice without releasing');
        held.add(id);
      } else if (c.command.kind == CommandKind.keyRelease) {
        expect(held.remove(id), isTrue,
            reason: 'released ${key.wire} that was never pressed');
      }
    }

    expect(held, isEmpty, reason: 'these keys were left down: $held');
  });

  test('a release and the next press never share a timestamp', () async {
    await run();

    // The contract: "the old gesture emits ended on one frame and the new
    // one emits began on a LATER frame. Never the same frame." Dispatching
    // both together would let a key_release and a key_press go out of order.
    for (var i = 1; i < commands.length; i++) {
      final before = commands[i - 1];
      final now = commands[i];
      if (before.phase != GesturePhase.ended) continue;
      if (now.phase != GesturePhase.began) continue;
      if (before.handedness != now.handedness) continue;

      expect(now.at.isAfter(before.at), isTrue,
          reason: '${before.gesture.wire} ended and ${now.gesture.wire} '
              'began on the same frame');
    }
  });

  test('both hands act in the same window', () async {
    await run();

    // The acceptance criterion for phase 2: an arrow held with the left hand
    // while the right works the cursor. If the mock never overlaps them, the
    // UI is never exercised on the thing the project is for.
    final left = commands.where((c) => c.handedness == Handedness.left);
    final right = commands.where((c) => c.handedness == Handedness.right);

    expect(left, isNotEmpty);
    expect(right, isNotEmpty);

    var overlapped = false;
    for (final press in left.where(
        (c) => c.command.kind == CommandKind.keyPress)) {
      final release = left.firstWhere(
        (c) =>
            c.command.kind == CommandKind.keyRelease &&
            c.command.key == press.command.key &&
            c.at.isAfter(press.at),
        orElse: () => press,
      );
      if (right.any((c) =>
          c.at.isAfter(press.at) && c.at.isBefore(release.at))) {
        overlapped = true;
        break;
      }
    }

    expect(overlapped, isTrue,
        reason: 'the right hand never acted while the left held a key');
  });

  test('an unlabelled hand is tracked but drives nothing by default',
      () async {
    await run();

    // Handedness confidence dips below the line on purpose, so the UI has
    // an unlabelled hand to render.
    expect(
      frames.any((f) =>
          f.hands.any((h) => h.handedness == Handedness.unknown)),
      isTrue,
      reason: 'the mock never produced an unlabelled hand',
    );

    // And it drives NOTHING, because the shipped configuration binds only
    // `left` and `right`. This is not a mock quirk — it is the reason
    // HandSelector.any exists, and the reason the editor offers it first.
    expect(
      commands.any((c) => c.handedness == Handedness.unknown),
      isFalse,
      reason: 'an unlabelled hand matched a left/right binding',
    );
  });

  test('commands are produced even though nothing is delivered', () async {
    await run(forFrames: 120);

    // A sink-less controller is a supported configuration, not a degraded
    // one. Commands are data; delivery is a separate question.
    expect(commands, isNotEmpty);
    expect(commands.every((c) => !c.delivered), isTrue);
  });

  test('pausing releases what is held', () async {
    await run(forFrames: 400);
    await engine.setPaused(true);

    final held = <HcKey>{};
    for (final c in commands) {
      final key = c.command.key;
      if (key == null) continue;
      if (c.command.kind == CommandKind.keyPress) {
        held.add(key);
      } else {
        held.remove(key);
      }
    }

    // A key held across a pause is the same stuck key as one held across a
    // shutdown.
    expect(held, isEmpty, reason: 'pause left these down: $held');
  });

  test('asking to inject does not mean it happened', () async {
    await run(forFrames: 60);
    await engine.setInjectInput(true);

    // XTest returns success and moves nothing. The engine must report the
    // refusal rather than the request.
    expect(engine.state.value.sink, SinkStatus.unavailable);
    expect(engine.state.value.sinkReason, isNotNull);
  });
}
