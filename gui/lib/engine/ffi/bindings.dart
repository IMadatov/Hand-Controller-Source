/// Dart's side of `cpp/ffi/include/hc/hand_controller.h`.
///
/// Every struct below mirrors a C struct FIELD FOR FIELD AND IN ORDER. A
/// reordered or missing field is not a compile error on either side — it is a
/// silent misread at runtime, which is exactly what [HC_ABI_VERSION] exists
/// to catch. Check the header before touching anything here.
///
/// The enum integer values are part of the ABI; the C++ side pins each with a
/// static_assert. They are not re-declared here: the Dart domain enums own
/// the names, and [hcGestureFromAbi] and friends do the mapping in one place.
library;

import 'dart:ffi';

/// Fixed by the model, not a preference.
const int kHcLandmarkCount = 21;

/// Must equal `hc_abi_version()` from the loaded library.
const int kHcAbiVersion = 1;

// --- enums, as plain ints ---------------------------------------------------

abstract final class HcSource {
  static const camera = 0;
  static const video = 1;

  /// Generated frames: no camera and no hands. For running the plumbing end
  /// to end before a camera exists.
  static const synthetic = 2;
}

abstract final class HcSink {
  /// A supported configuration, not a degraded one — commands are produced
  /// and reported, and nothing is moved.
  static const none = 0;
  static const log = 1;

  /// A virtual input device in the kernel, so it works under Wayland and X11
  /// alike. Needs one-time access to `/dev/uinput`.
  static const uinput = 2;
}

// --- per-frame data ---------------------------------------------------------

final class HcPoint extends Struct {
  @Float()
  external double x;

  @Float()
  external double y;
}

final class HcHand extends Struct {
  @Uint32()
  external int trackId;

  @Int32()
  external int handedness;

  @Float()
  external double handednessScore;

  @Float()
  external double presence;

  @Int64()
  external int timestampUs;

  @Array(kHcLandmarkCount)
  external Array<HcPoint> landmarks;
}

final class HcEvent extends Struct {
  @Uint32()
  external int trackId;

  /// Latched at `began`, not re-read per frame.
  @Int32()
  external int handedness;

  @Int32()
  external int gesture;

  @Int32()
  external int phase;

  external HcPoint position;
  external HcPoint delta;

  @Int64()
  external int timestampUs;
}

final class HcCommand extends Struct {
  @Int32()
  external int kind;

  external HcPoint delta;

  @Int32()
  external int button;

  @Int32()
  external int key;

  /// Index into this snapshot's events of the gesture that caused this, or
  /// -1. A command cannot explain itself; this is how a log row knows which
  /// hand and which gesture produced it.
  @Int32()
  external int originEvent;

  /// 1 when a sink actually received it. 0 in preview mode and while paused.
  @Int32()
  external int delivered;
}

/// One stepped frame.
///
/// The caller fills the three pointers and the three capacities; `poll`
/// writes the three counts. A count equal to its capacity means output may
/// have been truncated.
final class HcSnapshot extends Struct {
  external Pointer<HcHand> hands;
  @Int32()
  external int handCapacity;
  @Int32()
  external int handCount;

  external Pointer<HcEvent> events;
  @Int32()
  external int eventCapacity;
  @Int32()
  external int eventCount;

  external Pointer<HcCommand> commands;
  @Int32()
  external int commandCapacity;
  @Int32()
  external int commandCount;

  @Int64()
  external int timestampUs;

  /// Monotonic. THE SAME VALUE TWICE MEANS NO NEW FRAME — poll is not a
  /// queue, and a caller polling faster than the pipeline steps sees repeats.
  @Uint64()
  external int frameIndex;

  @Uint64()
  external int framesDropped;

  @Double()
  external double fps;

  @Double()
  external double latencyMs;

  @Int32()
  external int frameWidth;

  @Int32()
  external int frameHeight;
}

// --- session ----------------------------------------------------------------

final class HcSession extends Opaque {}

final class HcConfig extends Struct {
  @Int32()
  external int source;

  @Int32()
  external int cameraIndex;

  external Pointer<Uint8> videoPath;

  @Int32()
  external int syntheticWidth;

  @Int32()
  external int syntheticHeight;

  /// A CEILING, NOT A TARGET. The palm detector re-runs on every frame in
  /// which any slot is empty, so 2 in a one-hand scene costs a detection per
  /// frame.
  @Int32()
  external int maxHands;

  @Int32()
  external int threads;

  /// The bindings in the file format. Null means the configuration compiled
  /// into the library.
  external Pointer<Uint8> bindingsText;

  /// `<= 0` means "leave the text's own `set` line alone".
  @Float()
  external double sensitivity;

  @Float()
  external double smoothing;

  @Int32()
  external int sink;

  /// What a normalized delta of 1.0 is multiplied by. 0 means detect.
  @Int32()
  external int screenWidth;

  @Int32()
  external int screenHeight;
}

final class HcCamera extends Struct {
  @Int32()
  external int index;

  @Array(64)
  external Array<Uint8> path;

  @Array(128)
  external Array<Uint8> name;
}

// --- the loaded library -----------------------------------------------------

/// Every function this app calls, resolved once.
class HcLibrary {
  HcLibrary(this.dylib)
      : abiVersion = dylib.lookupFunction<Int32 Function(), int Function()>(
            'hc_abi_version'),
        configDefaults = dylib.lookupFunction<Void Function(Pointer<HcConfig>),
            void Function(Pointer<HcConfig>)>('hc_config_defaults'),
        sessionCreate = dylib.lookupFunction<
            Pointer<HcSession> Function(Pointer<HcConfig>, Pointer<Uint8>,
                Int32),
            Pointer<HcSession> Function(Pointer<HcConfig>, Pointer<Uint8>,
                int)>('hc_session_create'),
        sessionDestroy = dylib.lookupFunction<
            Void Function(Pointer<HcSession>),
            void Function(Pointer<HcSession>)>('hc_session_destroy'),
        sessionStart = dylib.lookupFunction<Int32 Function(Pointer<HcSession>),
            int Function(Pointer<HcSession>)>('hc_session_start'),
        sessionStop = dylib.lookupFunction<Void Function(Pointer<HcSession>),
            void Function(Pointer<HcSession>)>('hc_session_stop'),
        sessionRunning = dylib.lookupFunction<
            Int32 Function(Pointer<HcSession>),
            int Function(Pointer<HcSession>)>('hc_session_running'),
        sessionPoll = dylib.lookupFunction<
            Int32 Function(Pointer<HcSession>, Pointer<HcSnapshot>),
            int Function(Pointer<HcSession>,
                Pointer<HcSnapshot>)>('hc_session_poll'),
        sessionCopyFrame = dylib.lookupFunction<
            Int32 Function(Pointer<HcSession>, Pointer<Uint8>, Int32,
                Pointer<Int32>, Pointer<Int32>),
            int Function(Pointer<HcSession>, Pointer<Uint8>, int,
                Pointer<Int32>, Pointer<Int32>)>('hc_session_copy_frame'),
        sessionSetPaused = dylib.lookupFunction<
            Void Function(Pointer<HcSession>, Int32),
            void Function(Pointer<HcSession>, int)>('hc_session_set_paused'),
        sessionPaused = dylib.lookupFunction<Int32 Function(Pointer<HcSession>),
            int Function(Pointer<HcSession>)>('hc_session_paused'),
        sessionSetSink = dylib.lookupFunction<
            Int32 Function(Pointer<HcSession>, Int32, Pointer<Uint8>, Int32),
            int Function(Pointer<HcSession>, int, Pointer<Uint8>,
                int)>('hc_session_set_sink'),
        sessionSinkAvailable = dylib.lookupFunction<
            Int32 Function(Pointer<HcSession>),
            int Function(Pointer<HcSession>)>('hc_session_sink_available'),
        sessionBindingsText = dylib.lookupFunction<
            Int32 Function(Pointer<HcSession>, Pointer<Uint8>, Int32),
            int Function(Pointer<HcSession>, Pointer<Uint8>,
                int)>('hc_session_bindings_text'),
        sessionSetBindingsText = dylib.lookupFunction<
            Int32 Function(Pointer<HcSession>, Pointer<Uint8>, Pointer<Uint8>,
                Int32),
            int Function(Pointer<HcSession>, Pointer<Uint8>, Pointer<Uint8>,
                int)>('hc_session_set_bindings_text'),
        sessionLastError = dylib.lookupFunction<
            Int32 Function(Pointer<HcSession>, Pointer<Uint8>, Int32),
            int Function(Pointer<HcSession>, Pointer<Uint8>,
                int)>('hc_session_last_error'),
        defaultBindingsText = dylib.lookupFunction<
            Pointer<Uint8> Function(),
            Pointer<Uint8> Function()>('hc_default_bindings_text'),
        enumerateCameras = dylib.lookupFunction<
            Int32 Function(Pointer<HcCamera>, Int32),
            int Function(Pointer<HcCamera>, int)>('hc_enumerate_cameras');

  final DynamicLibrary dylib;

  final int Function() abiVersion;
  final void Function(Pointer<HcConfig>) configDefaults;
  final Pointer<HcSession> Function(Pointer<HcConfig>, Pointer<Uint8>, int)
      sessionCreate;
  final void Function(Pointer<HcSession>) sessionDestroy;
  final int Function(Pointer<HcSession>) sessionStart;
  final void Function(Pointer<HcSession>) sessionStop;
  final int Function(Pointer<HcSession>) sessionRunning;
  final int Function(Pointer<HcSession>, Pointer<HcSnapshot>) sessionPoll;
  final int Function(Pointer<HcSession>, Pointer<Uint8>, int, Pointer<Int32>,
      Pointer<Int32>) sessionCopyFrame;
  final void Function(Pointer<HcSession>, int) sessionSetPaused;
  final int Function(Pointer<HcSession>) sessionPaused;
  final int Function(Pointer<HcSession>, int, Pointer<Uint8>, int)
      sessionSetSink;
  final int Function(Pointer<HcSession>) sessionSinkAvailable;
  final int Function(Pointer<HcSession>, Pointer<Uint8>, int)
      sessionBindingsText;
  final int Function(Pointer<HcSession>, Pointer<Uint8>, Pointer<Uint8>, int)
      sessionSetBindingsText;
  final int Function(Pointer<HcSession>, Pointer<Uint8>, int) sessionLastError;
  final Pointer<Uint8> Function() defaultBindingsText;
  final int Function(Pointer<HcCamera>, int) enumerateCameras;
}
