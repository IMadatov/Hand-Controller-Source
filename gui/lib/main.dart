import 'package:flutter/material.dart';

import 'app.dart';
import 'engine/engine.dart';
import 'engine/mock_engine.dart';

/// Which engine the app runs against.
///
/// There is exactly one place in the app that names a concrete engine, and
/// this is it. When the C++ `Controller` gains its `extern "C"` shim, this
/// function gains an `FfiEngine` branch and nothing else in the app changes.
HandControllerEngine buildEngine() => MockEngine();

void main() {
  final engine = buildEngine();

  // Started immediately so the window is never a dead frame. Input injection
  // is OFF: a gesture app that seizes the cursor before the user has seen
  // what their hands do is not something you can recover from with a mouse.
  engine.start();

  runApp(HandControllerApp(engine: engine));
}
