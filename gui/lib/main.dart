import 'package:flutter/material.dart';

import 'app.dart';
import 'engine/engine.dart';
import 'engine/ffi/loader.dart';
import 'engine/ffi_engine.dart';
import 'engine/mock_engine.dart';

/// Which engine the app runs against.
///
/// THE ONLY PLACE IN THE APP THAT NAMES A CONCRETE ENGINE. Everything else
/// holds the interface, which is what made swapping the mock for the real
/// library a change to this function and nothing else.
///
/// The real library is preferred and the mock is the fallback, not the other
/// way round: a developer who has the `.so` should be running against it
/// without opting in, and the fallback exists so the UI still runs where the
/// library is missing — a fresh clone, a machine with no build tree, a
/// widget test.
HandControllerEngine buildEngine() {
  try {
    return FfiEngine.open();
  } on HcLibraryUnavailable catch (e) {
    // Said out loud rather than swallowed. Silently running on invented data
    // while believing it is real is the worst of the three outcomes.
    debugPrint('Falling back to MockEngine: $e');
    return MockEngine();
  }
}

void main() {
  final engine = buildEngine();

  // Started immediately so the window is never a dead frame. Input injection
  // is OFF: a gesture app that seizes the cursor before the user has seen
  // what their hands do is not something you can recover from with a mouse.
  engine.start();

  runApp(HandControllerApp(engine: engine));
}
