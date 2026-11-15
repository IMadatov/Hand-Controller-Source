/// Renders each screen to a PNG under `test/goldens/`.
///
/// This is how the UI is reviewed on this machine: the session is Wayland
/// and no screenshot utility is installed, so running the app and grabbing
/// the window is not available. Rendering through the test harness is also
/// repeatable, which a hand-taken screenshot is not.
///
/// Refresh with:
///   flutter test --update-goldens test/screenshots_test.dart
///
/// The engine is deterministic — fixed frame interval, no randomness, and an
/// injected clock — so these do hold as assertions, and a diff means the UI
/// changed. They are still primarily REVIEW ARTIFACTS: when a change is
/// intended, look at the new PNG before running `--update-goldens`, rather
/// than updating reflexively.
library;

import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:hand_controller_gui/app.dart';
import 'package:hand_controller_gui/engine/mock_engine.dart';
import 'package:hand_controller_gui/screens/onboarding.dart';
import 'package:hand_controller_gui/screens/tray_overlay.dart';

/// The canvas every mockup was designed on.
const _windowSize = Size(1280, 800);

/// Loads the bundled faces into the test harness.
///
/// Without this, text renders as the test font's filled boxes and the
/// screenshots are useless for judging layout.
Future<void> _loadFonts() async {
  // MaterialIcons ships with the SDK, not with this package, so the harness
  // does not pick it up from `uses-material-design`. Without it every icon
  // renders as an empty box and the screenshots misrepresent the UI.
  final iconFont = _findIconFont();

  for (final (family, path) in [
    ('Inter', 'assets/fonts/Inter.ttf'),
    ('Space Grotesk', 'assets/fonts/SpaceGrotesk.ttf'),
    if (iconFont != null) ('MaterialIcons', iconFont.path),
  ]) {
    final loader = FontLoader(family)
      ..addFont(File(path).readAsBytes().then(ByteData.sublistView));
    await loader.load();
  }
}

/// Walks up from the test runner binary looking for the SDK's font cache.
///
/// Derived rather than hardcoded because the SDK lives wherever the
/// developer put it; returns null if the layout ever changes, in which case
/// icons render as boxes and the screenshots are merely less useful.
File? _findIconFont() {
  var dir = File(Platform.resolvedExecutable).parent;
  for (var i = 0; i < 8; i++) {
    final candidate = File(
      '${dir.path}/cache/artifacts/material_fonts/MaterialIcons-Regular.otf',
    );
    if (candidate.existsSync()) return candidate;
    if (dir.parent.path == dir.path) break;
    dir = dir.parent;
  }
  return null;
}

/// Pumps enough frames for a route transition to finish.
///
/// `pumpAndSettle` is not usable here: the engine's frame timer never stops,
/// so there is no quiet moment to settle into.
Future<void> settle(WidgetTester tester) async {
  for (var i = 0; i < 12; i++) {
    await tester.pump(const Duration(milliseconds: 50));
  }
}

/// Closes the top route from the root navigator.
///
/// `skipOffstage: false` because a full-screen route takes the one below it
/// offstage, and the shell is exactly what we need a context from.
Future<void> closeRoute(WidgetTester tester) async {
  Navigator.of(tester.element(find.byType(AppShell, skipOffstage: false)))
      .pop();
  await settle(tester);
}

void main() {
  setUpAll(_loadFonts);

  testWidgets('screens', (tester) async {
    await tester.binding.setSurfaceSize(_windowSize);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(() => tester.binding.setSurfaceSize(null));

    // A fixed interval, no randomness, and a clock derived from the frame
    // count means every pixel below is a function of the pump schedule. The
    // command log would otherwise carry wall-clock timestamps and these
    // screenshots would differ on every run.
    var tick = 0;
    final engine = MockEngine(
      frameInterval: const Duration(milliseconds: 35),
      clock: () => DateTime.utc(2026, 1, 1, 9, 30)
          .add(Duration(milliseconds: 35 * tick++)),
    );
    engine.start();

    await tester.pumpWidget(HandControllerApp(engine: engine));

    // Past the simulated camera-open delay, then far enough into the script
    // that both hands are mid-gesture and the command log has filled.
    await tester.pump(const Duration(milliseconds: 600));
    for (var i = 0; i < 70; i++) {
      await tester.pump(const Duration(milliseconds: 35));
    }

    await expectLater(
      find.byType(HandControllerApp),
      matchesGoldenFile('goldens/live_view.png'),
    );

    // Every other section, so an unbuilt screen is visible as unbuilt.
    for (final section in [
      AppSection.bindings,
      AppSection.settings,
      AppSection.diagnostics,
    ]) {
      await tester.tap(find.text(section.label));

      // Twice. A screen's startup work is kicked off DURING the pump that
      // mounts it — Settings and Diagnostics both enumerate cameras — so one
      // pump captures the loading state rather than the screen.
      await tester.pump(const Duration(milliseconds: 400));
      await tester.pump(const Duration(milliseconds: 400));

      await expectLater(
        find.byType(HandControllerApp),
        matchesGoldenFile('goldens/${section.name}.png'),
      );
    }

    // The two screens that are not in the sidebar: the first-run wizard and
    // the compact overlay, both entered from Settings.
    await tester.tap(find.text(AppSection.settings.label));
    await tester.pump(const Duration(milliseconds: 400));
    await tester.pump(const Duration(milliseconds: 400));

    // Both entry points live under the System category, not the Camera one
    // Settings opens on.
    await tester.tap(find.text('System'));
    await tester.pump(const Duration(milliseconds: 200));

    await tester.tap(find.text('Show overlay'));
    await settle(tester);
    await expectLater(
      find.byType(HandControllerApp),
      matchesGoldenFile('goldens/tray_overlay.png'),
    );
    expect(find.byType(TrayOverlay), findsOneWidget);
    await closeRoute(tester);

    await tester.tap(find.text('Run setup'));
    await settle(tester);
    await expectLater(
      find.byType(HandControllerApp),
      matchesGoldenFile('goldens/onboarding.png'),
    );

    // Step 2, where the calibration measurement is actually running.
    await tester.tap(find.text('Continue'));
    for (var i = 0; i < 40; i++) {
      await tester.pump(const Duration(milliseconds: 35));
    }
    await expectLater(
      find.byType(HandControllerApp),
      matchesGoldenFile('goldens/onboarding_calibrate.png'),
    );

    expect(find.byType(OnboardingScreen), findsOneWidget);
    await closeRoute(tester);
    await tester.tap(find.text(AppSection.diagnostics.label));
    await tester.pump(const Duration(milliseconds: 400));
    await tester.pump(const Duration(milliseconds: 400));

    // Diagnostics again with injection requested and refused, which is the
    // state this screen exists for: XTest reports success and moves nothing,
    // so a user whose gestures do nothing is sent here to find out why.
    await engine.setInjectInput(true);
    await tester.pump(const Duration(milliseconds: 100));
    await expectLater(
      find.byType(HandControllerApp),
      matchesGoldenFile('goldens/diagnostics_input_unavailable.png'),
    );

    // A screen's startup work is kicked off DURING the pump that mounts it,
    // so it is still outstanding when that pump returns. One more drains
    // whatever the last section started.
    await tester.pump(const Duration(milliseconds: 400));

    await engine.dispose();
  });
}
