/// Exercises [FfiEngine] against the real `libhand_controller.so`.
///
/// SKIPPED where the library is absent — a fresh clone, or a machine with no
/// build tree — rather than failing. The suite has to stay green for someone
/// who is only touching the UI.
///
/// What is checked here is the BOUNDARY, not the pipeline: that the structs
/// line up, that counts and capacities behave as documented, and that the
/// three hazards which bite silently are honoured. Whether the tracker finds
/// a hand is the C++ side's business and is tested there.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:hand_controller_gui/domain/observation.dart';
import 'package:hand_controller_gui/engine/engine.dart';
import 'package:hand_controller_gui/engine/ffi/loader.dart';
import 'package:hand_controller_gui/engine/ffi_engine.dart';

void main() {
  late FfiEngine engine;
  String? unavailable;

  setUp(() {
    try {
      engine = FfiEngine.open();
    } on HcLibraryUnavailable catch (e) {
      unavailable = e.message;
    }
  });

  tearDown(() async {
    if (unavailable == null) await engine.dispose();
  });


  test('the library opens and its ABI matches', () {
    if (unavailable != null) {
      markTestSkipped('no library: $unavailable');
      return;
    }
    // Reaching here means openHcLibrary accepted hc_abi_version().
    expect(engine, isNotNull);
  });

  test('the compiled-in bindings parse and are valid', () async {
    if (unavailable != null) {
      markTestSkipped('no library: $unavailable');
      return;
    }

    // Read before start, so this is hc_default_bindings_text.
    final defaults = await engine.loadBindings();

    expect(defaults.bindings, isNotEmpty);
    expect(defaults.isValid, isTrue,
        reason: defaults.problems.map((p) => p.message).join('; '));
    expect(defaults.sensitivity, greaterThan(0));
    expect(defaults.smoothing, inInclusiveRange(0, 1));

    // Every key the shipped configuration holds, it also releases.
    expect(defaults.unmatchedKeyPresses, isEmpty);
  });

  test('camera enumeration does not overrun its buffer', () async {
    if (unavailable != null) {
      markTestSkipped('no library: $unavailable');
      return;
    }

    final cameras = await engine.enumerateCameras();

    // An empty list is a valid answer; this machine may have no capture
    // device. What matters is that the strings came back intact rather than
    // as garbage from a misread struct.
    for (final camera in cameras) {
      expect(camera.path, startsWith('/dev/'));
      expect(camera.name, isNotEmpty);
    }
  });

  test('asking to inject reports what actually happened', () async {
    if (unavailable != null) {
      markTestSkipped('no library: $unavailable');
      return;
    }

    await engine.start();
    if (engine.state.value.lifecycle == EngineLifecycle.failed) {
      markTestSkipped('could not start: ${engine.state.value.fault}');
      return;
    }

    expect(engine.state.value.sink, SinkStatus.observeOnly);

    await engine.setInjectInput(true);

    // Either it worked or it did not — both are correct outcomes here. What
    // must never happen is `active` without the sink actually being
    // available, which is the Wayland trap this whole path exists for.
    final state = engine.state.value;
    expect(state.sink, isNot(SinkStatus.observeOnly));
    if (state.sink == SinkStatus.unavailable) {
      expect(state.sinkReason, isNotNull);
      expect(state.sinkReason, isNotEmpty);
    }

    await engine.stop();
  });

  test('frames arrive and carry a moving frame index', () async {
    if (unavailable != null) {
      markTestSkipped('no library: $unavailable');
      return;
    }

    final seen = <EngineFrame>[];
    final sub = engine.frames.listen(seen.add);

    await engine.start();
    if (engine.state.value.lifecycle == EngineLifecycle.failed) {
      markTestSkipped('could not start: ${engine.state.value.fault}');
      await sub.cancel();
      return;
    }

    await Future<void>.delayed(const Duration(seconds: 3));
    await sub.cancel();

    if (seen.isEmpty) {
      // No frame source on this machine. Not a boundary failure.
      markTestSkipped('no frames; the source produced nothing');
      return;
    }

    // Poll runs at ~60 Hz and the pipeline steps far slower, so duplicates
    // are the norm at the C level. None may reach here: a repeated frame is
    // dropped on the frame_index check, and without that the UI would
    // re-render identical data at the poll rate.
    final timestamps = seen.map((f) => f.timestampUs).toList();
    expect(timestamps.toSet().length, timestamps.length,
        reason: 'the same frame was published twice');

    for (final frame in seen) {
      for (final hand in frame.hands) {
        expect(hand.landmarks, hasLength(kLandmarkCount));
        for (final point in hand.landmarks) {
          // Normalized, never pixels. A value far outside 0..1 means the
          // struct was misread.
          expect(point.x, inInclusiveRange(-1, 2));
          expect(point.y, inInclusiveRange(-1, 2));
        }
        expect(hand.handednessScore, inInclusiveRange(0, 1));
        expect(hand.presence, inInclusiveRange(0, 1));
      }
    }

    expect(engine.truncations, 0,
        reason: 'a poll buffer was too small and output was truncated');

    await engine.stop();
  });
}
