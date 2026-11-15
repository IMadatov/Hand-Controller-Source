/// Seed data for [MockEngine], transcribed from `cpp/config/default_bindings.txt`.
///
/// THIS IS MOCK DATA, NOT THE SOURCE OF TRUTH. The real defaults ship with
/// the engine and arrive through `loadBindings()`; `FfiEngine` will never
/// read this file. It exists so the UI has something coherent to render
/// before the FFI boundary exists, and it is the one place in this app that
/// knowingly duplicates something C++ owns.
library;

import '../domain/bindings.dart';
import '../domain/command.dart';
import '../domain/gesture.dart';

/// A press plus the release that must accompany it.
///
/// Every arrow in the default set is HELD for as long as the pose is, rather
/// than tapped. Holding is what the owner asked for — an arrow down with the
/// left hand while the right hand works the cursor — and it is the
/// acceptance criterion for independent per-hand state.
List<Binding> _held(GestureKind gesture, HcKey key) {
  final press = Binding(
    hand: HandSelector.left,
    gesture: gesture,
    phase: GesturePhase.began,
    command: HcCommand(kind: CommandKind.keyPress, key: key),
  );
  return [press, BindingSet.releaseFor(press)];
}

BindingSet mockDefaultBindings() => BindingSet(
      sensitivity: kDefaultSensitivity,
      smoothing: kDefaultSmoothing,
      bindings: [
        // --- Right hand: the pointer ------------------------------------

        // `held`, so it acts on every frame the pose lasts.
        const Binding(
          hand: HandSelector.right,
          gesture: GestureKind.pointIndex,
          phase: GesturePhase.held,
          command: HcCommand(kind: CommandKind.moveCursor),
        ),
        const Binding(
          hand: HandSelector.right,
          gesture: GestureKind.openPalm,
          phase: GesturePhase.held,
          command: HcCommand(kind: CommandKind.scroll),
        ),

        // `began`, so it fires once no matter how long the pose is held.
        const Binding(
          hand: HandSelector.right,
          gesture: GestureKind.twoFingers,
          phase: GesturePhase.began,
          command: HcCommand(kind: CommandKind.click),
        ),

        // A pinch is measured from the distance between two fingertips, so it
        // needs no thumb-extension threshold and cannot be confused with
        // pointing. main.py used thumb-and-index-both-up, which differed from
        // its cursor pose only by the least reliable landmark on the hand.
        const Binding(
          hand: HandSelector.right,
          gesture: GestureKind.pinch,
          phase: GesturePhase.began,
          command: HcCommand(kind: CommandKind.doubleClick),
        ),

        // --- Left hand: arrows and the screenshot -----------------------
        ..._held(GestureKind.thumbUp, HcKey.arrowRight),
        ..._held(GestureKind.pointIndex, HcKey.arrowUp),
        ..._held(GestureKind.pinkyOnly, HcKey.arrowLeft),
        ..._held(GestureKind.fourFingers, HcKey.arrowDown),

        // The screenshot happens on the press; the release exists so the key
        // does not stay down.
        ..._held(GestureKind.openPalm, HcKey.printScreen),
      ],
    );
