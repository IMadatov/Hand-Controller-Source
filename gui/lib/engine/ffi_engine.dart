/// The real engine: [HandControllerEngine] over `libhand_controller.so`.
///
/// Reads as a sibling of `MockEngine` on purpose. No widget knows which one
/// it has, and the only place in the app that names either is
/// `main.dart`'s `buildEngine()`.
library;

import 'dart:async';
import 'dart:ffi';

import 'package:flutter/foundation.dart';

import '../domain/bindings.dart';
import '../domain/command.dart';
import '../domain/gesture.dart';
import '../domain/observation.dart';
import 'engine.dart';
import 'ffi/bindings.dart' as abi;
import 'ffi/bindings_text.dart';
import 'ffi/loader.dart';

/// Buffer sizes for one poll.
///
/// The caller owns every buffer and truncation is VISIBLE, not silent: a
/// count equal to its capacity means there may have been more, and
/// [FfiEngine.truncations] counts those so a wrong number here surfaces as a
/// reported fault rather than as quietly missing commands.
const _handCapacity = 4;
const _eventCapacity = 32;
const _commandCapacity = 64;

const _errorCapacity = 1024;

class FfiEngine implements HandControllerEngine {
  FfiEngine._(this._lib, this._arena);

  /// Opens the library and checks its ABI version.
  ///
  /// Throws [HcLibraryUnavailable] if it is missing or mismatched. The caller
  /// decides what to do about that; `main.dart` falls back to the mock.
  factory FfiEngine.open({String? libraryPath}) {
    final lib = openHcLibrary(overridePath: libraryPath);
    return FfiEngine._(lib, _Arena());
  }

  final abi.HcLibrary _lib;
  final _Arena _arena;

  Pointer<abi.HcSession> _session = nullptr;
  Timer? _timer;

  final _state = ValueNotifier<EngineState>(EngineState.initial);
  final _frames = StreamController<EngineFrame>.broadcast();

  /// The last frame_index handed back by the library.
  ///
  /// POLL IS NOT A QUEUE. It reports the latest published snapshot, so
  /// polling faster than the pipeline steps returns the same frame again.
  /// Rebuilding on a repeat would re-render identical data at the poll rate
  /// rather than the frame rate.
  int _lastFrameIndex = 0;

  /// How many polls came back with a count equal to its capacity.
  int truncations = 0;

  /// Whether the user has asked for injection, which is NOT whether it is
  /// happening. See [setInjectInput].
  bool _injectRequested = false;

  @override
  ValueListenable<EngineState> get state => _state;

  @override
  Stream<EngineFrame> get frames => _frames.stream;

  @override
  Future<List<CameraDevice>> enumerateCameras() async {
    const capacity = 8;
    final buffer = _arena.allocate<abi.HcCamera>(sizeOf<abi.HcCamera>() * capacity);
    final found = _lib.enumerateCameras(buffer, capacity);

    // The count may EXCEED the capacity — that is the API telling us to size
    // up rather than pretending there were only eight.
    final readable = found < capacity ? found : capacity;

    final cameras = <CameraDevice>[];
    for (var i = 0; i < readable; i++) {
      final camera = (buffer + i).ref;
      cameras.add(CameraDevice(
        id: '${camera.index}',
        name: fromCharArray(camera.name, 128),
        path: fromCharArray(camera.path, 64),
      ));
    }
    _arena.free(buffer);
    return cameras;
  }

  @override
  Future<void> start({CameraDevice? camera, bool injectInput = false}) async {
    if (_session != nullptr) await stop();

    _state.value = _state.value.copyWith(
      lifecycle: EngineLifecycle.starting,
      camera: camera,
      fault: null,
    );

    final config = _arena.allocate<abi.HcConfig>(sizeOf<abi.HcConfig>());

    // ALWAYS defaults first: a field added in a later version would
    // otherwise arrive as whatever was on the stack.
    _lib.configDefaults(config);

    config.ref.source = abi.HcSource.camera;
    config.ref.cameraIndex = int.tryParse(camera?.id ?? '') ?? 0;

    // Injection is never requested at create time. The user turns it on
    // deliberately, and a refusal then is a message rather than a failed
    // start.
    config.ref.sink = abi.HcSink.none;

    final error = _arena.allocate<Uint8>(_errorCapacity);
    final session = _lib.sessionCreate(config, error, _errorCapacity);
    _arena.free(config);

    if (session == nullptr) {
      final reason = fromNativeUtf8(error, _errorCapacity);
      _arena.free(error);
      _state.value = _state.value.copyWith(
        lifecycle: EngineLifecycle.failed,
        fault: reason.isEmpty ? 'The session could not be created.' : reason,
      );
      return;
    }
    _arena.free(error);
    _session = session;

    if (_lib.sessionStart(_session) != 1) {
      _state.value = _state.value.copyWith(
        lifecycle: EngineLifecycle.failed,
        fault: _lastError(),
      );
      return;
    }

    _lastFrameIndex = 0;
    truncations = 0;
    _injectRequested = false;
    _state.value = _state.value.copyWith(lifecycle: EngineLifecycle.running);
    _refreshSinkState();

    if (injectInput) await setInjectInput(true);

    // THE FRAME LOOP IS ALREADY ON A THREAD INSIDE THE LIBRARY. Step() costs
    // ~11 ms and no UI isolate can pay that 90 times a second, so the worker
    // runs in C++ and this only copies out the latest snapshot. That is a
    // memcpy; doing it from a background isolate would add a copy and a
    // message hop for nothing.
    _timer = Timer.periodic(const Duration(milliseconds: 16), (_) => _poll());
  }

  @override
  Future<void> stop() async {
    _timer?.cancel();
    _timer = null;

    if (_session != nullptr) {
      // Releases every held key on the way out.
      _lib.sessionStop(_session);
      _lib.sessionDestroy(_session);
      _session = nullptr;
    }

    _state.value = _state.value.copyWith(
      lifecycle: EngineLifecycle.stopped,
      sink: SinkStatus.observeOnly,
      sinkReason: null,
    );
  }

  @override
  Future<void> setPaused(bool paused) async {
    if (_session == nullptr) return;

    // Entering pause releases whatever the sink holds — the library does
    // this, not us. Leaving pause presses nothing again.
    _lib.sessionSetPaused(_session, paused ? 1 : 0);
    _state.value = _state.value.copyWith(
      lifecycle: paused ? EngineLifecycle.paused : EngineLifecycle.running,
    );
  }

  @override
  Future<void> setInjectInput(bool inject) async {
    if (_session == nullptr) return;

    final error = _arena.allocate<Uint8>(_errorCapacity);
    final ok = _lib.sessionSetSink(
      _session,
      inject ? abi.HcSink.uinput : abi.HcSink.none,
      error,
      _errorCapacity,
    );

    if (ok == 1) {
      _injectRequested = inject;
      _arena.free(error);
      _refreshSinkState();
      return;
    }

    // A FAILED set_sink KEEPS THE PREVIOUS SINK. Asking to inject is not
    // succeeding, so the request is not recorded and the toggle is left
    // where the engine actually is. The message carries the udev rule that
    // fixes it and is shown verbatim rather than summarised.
    final reason = fromNativeUtf8(error, _errorCapacity);
    _arena.free(error);

    _refreshSinkState();
    _state.value = _state.value.copyWith(
      sink: SinkStatus.unavailable,
      sinkReason: reason.isEmpty ? _lastError() : reason,
    );
  }

  /// Reads the sink state back out of the engine rather than assuming it.
  ///
  /// `sink_available` answers whether the ATTACHED SINK CAN DELIVER, not
  /// whether it injects — a logging sink is available and moves no cursor.
  /// Which sink was chosen is this side's own knowledge, so the two are
  /// combined here and nowhere else.
  void _refreshSinkState() {
    if (_session == nullptr) return;

    final available = _lib.sessionSinkAvailable(_session) == 1;
    _state.value = _state.value.copyWith(
      sink: switch ((_injectRequested, available)) {
        (false, _) => SinkStatus.observeOnly,
        (true, true) => SinkStatus.active,
        (true, false) => SinkStatus.unavailable,
      },
      sinkReason: null,
    );
  }

  void _poll() {
    if (_session == nullptr) return;

    final snapshot = _arena.allocate<abi.HcSnapshot>(sizeOf<abi.HcSnapshot>());
    final hands = _arena.allocate<abi.HcHand>(sizeOf<abi.HcHand>() * _handCapacity);
    final events = _arena.allocate<abi.HcEvent>(sizeOf<abi.HcEvent>() * _eventCapacity);
    final commands =
        _arena.allocate<abi.HcCommand>(sizeOf<abi.HcCommand>() * _commandCapacity);

    snapshot.ref
      ..hands = hands
      ..handCapacity = _handCapacity
      ..handCount = 0
      ..events = events
      ..eventCapacity = _eventCapacity
      ..eventCount = 0
      ..commands = commands
      ..commandCapacity = _commandCapacity
      ..commandCount = 0;

    try {
      if (_lib.sessionPoll(_session, snapshot) != 1) return;

      final s = snapshot.ref;

      // The same index twice means no new frame arrived. Returning here is
      // the difference between rendering at the frame rate and rendering at
      // the poll rate with identical data.
      if (s.frameIndex == _lastFrameIndex) return;
      _lastFrameIndex = s.frameIndex;

      if (s.handCount == _handCapacity ||
          s.eventCount == _eventCapacity ||
          s.commandCount == _commandCapacity) {
        truncations++;
      }

      _frames.add(_readFrame(s));
    } finally {
      _arena
        ..free(commands)
        ..free(events)
        ..free(hands)
        ..free(snapshot);
    }
  }

  EngineFrame _readFrame(abi.HcSnapshot s) {
    final hands = <HandObservation>[];
    for (var i = 0; i < s.handCount; i++) {
      final hand = (s.hands + i).ref;
      hands.add(HandObservation(
        trackId: hand.trackId,
        handedness: _handedness(hand.handedness),
        handednessScore: hand.handednessScore,
        presence: hand.presence,
        landmarks: [
          for (var p = 0; p < abi.kHcLandmarkCount; p++)
            (x: hand.landmarks[p].x, y: hand.landmarks[p].y),
        ],
        timestampUs: hand.timestampUs,
      ));
    }

    final events = <GestureEvent>[];
    for (var i = 0; i < s.eventCount; i++) {
      final event = (s.events + i).ref;
      events.add(GestureEvent(
        trackId: event.trackId,
        handedness: _handedness(event.handedness),
        kind: GestureKind.values[event.gesture],
        phase: GesturePhase.values[event.phase],
        position: (x: event.position.x, y: event.position.y),
        delta: (x: event.delta.x, y: event.delta.y),
        timestampUs: event.timestampUs,
      ));
    }

    final at = DateTime.now();
    final commands = <CommandRecord>[];
    for (var i = 0; i < s.commandCount; i++) {
      final command = (s.commands + i).ref;

      // A command cannot explain itself. `origin_event` is how a log row
      // knows which hand and which gesture produced it.
      final origin = command.originEvent >= 0 &&
              command.originEvent < events.length
          ? events[command.originEvent]
          : null;

      commands.add(CommandRecord(
        command: HcCommand(
          kind: CommandKind.values[command.kind],
          delta: (x: command.delta.x, y: command.delta.y),
          button: MouseButton.values[command.button],
          key: command.kind == CommandKind.keyPress.index ||
                  command.kind == CommandKind.keyRelease.index
              ? HcKey.values[command.key]
              : null,
        ),
        handedness: origin?.handedness ?? Handedness.unknown,
        gesture: origin?.kind ?? GestureKind.none,
        phase: origin?.phase ?? GesturePhase.began,
        at: at,
        delivered: command.delivered == 1,
      ));
    }

    return EngineFrame(
      hands: hands,
      events: events,
      commands: commands,
      metrics: EngineMetrics(
        fps: s.fps,
        latencyMs: s.latencyMs,
        droppedFrames: s.framesDropped,
      ),
      timestampUs: s.timestampUs,
    );
  }

  static Handedness _handedness(int value) => switch (value) {
        1 => Handedness.left,
        2 => Handedness.right,
        _ => Handedness.unknown,
      };

  @override
  Future<BindingSet> loadBindings() async {
    if (_session == nullptr) {
      return parseBindingsText(
          fromNativeUtf8(_lib.defaultBindingsText(), 1 << 16));
    }
    return parseBindingsText(_bindingsText());
  }

  @override
  Future<void> saveBindings(BindingSet set) async {
    if (_session == nullptr) {
      throw BindingsRejected(const ['The engine is not running.']);
    }

    final text = toNativeUtf8(serializeBindingsText(set), _arena);
    final error = _arena.allocate<Uint8>(_errorCapacity);

    final ok = _lib.sessionSetBindingsText(_session, text, error,
        _errorCapacity);
    final reason = fromNativeUtf8(error, _errorCapacity);
    _arena
      ..free(error)
      ..free(text);

    if (ok != 1) {
      // A rejected edit LEAVES THE PREVIOUS BINDINGS IN PLACE, so the
      // running configuration is still the working one. The parser names the
      // offending line; pass it through unedited.
      throw BindingsRejected([reason.isEmpty ? _lastError() : reason]);
    }
  }

  /// The engine's own serialization of what it now holds.
  String _bindingsText() {
    final needed = _lib.sessionBindingsText(_session, nullptr, 0);
    if (needed <= 0) return '';

    final buffer = _arena.allocate<Uint8>(needed);
    _lib.sessionBindingsText(_session, buffer, needed);
    final text = fromNativeUtf8(buffer, needed);
    _arena.free(buffer);
    return text;
  }

  String _lastError() {
    if (_session == nullptr) return '';
    final buffer = _arena.allocate<Uint8>(_errorCapacity);
    _lib.sessionLastError(_session, buffer, _errorCapacity);
    final message = fromNativeUtf8(buffer, _errorCapacity);
    _arena.free(buffer);
    return message;
  }

  @override
  Future<void> dispose() async {
    await stop();
    await _frames.close();
    _state.dispose();
  }
}

/// Native allocation, using Dart's own malloc rather than `package:ffi`.
class _Arena implements Allocator {
  @override
  Pointer<T> allocate<T extends NativeType>(int byteCount, {int? alignment}) =>
      // ignore: omit_local_variable_types
      calloc(byteCount).cast<T>();

  @override
  void free(Pointer<NativeType> pointer) => _free(pointer.cast<Void>());

  static final _calloc = DynamicLibrary.process().lookupFunction<
      Pointer<Void> Function(IntPtr, IntPtr),
      Pointer<Void> Function(int, int)>('calloc');

  static final _free = DynamicLibrary.process()
      .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>(
          'free');

  static Pointer<Void> calloc(int byteCount) => _calloc(1, byteCount);
}
