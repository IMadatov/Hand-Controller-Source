import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app.dart';
import '../domain/gesture.dart';
import '../domain/observation.dart';
import '../engine/engine.dart';
import '../theme/tokens.dart';
import '../widgets/common.dart';
import '../widgets/hand_overlay.dart';

enum _Step {
  camera('Camera', 'Pick a camera'),
  calibrate('Calibrate', 'Calibrate your hand'),
  gestures('Gestures', 'Try the gestures'),
  start('Start', 'Ready');

  const _Step(this.label, this.title);

  final String label;
  final String title;
}

/// The first-run wizard.
///
/// Entered from Settings rather than automatically, because "is this the
/// first run" needs a stored flag and nothing is persisted yet. When there is
/// a store, the only change is who pushes this route.
class OnboardingScreen extends StatefulWidget {
  const OnboardingScreen({super.key});

  static Route<void> route() => MaterialPageRoute(
        builder: (_) => const OnboardingScreen(),
        fullscreenDialog: true,
      );

  @override
  State<OnboardingScreen> createState() => _OnboardingScreenState();
}

class _OnboardingScreenState extends State<OnboardingScreen> {
  StreamSubscription<EngineFrame>? _sub;

  var _step = _Step.camera;
  EngineFrame? _frame;
  final _gestures = <int, GestureKind>{};

  /// Poses the user has successfully made, for the gestures step.
  final _demonstrated = <GestureKind>{};

  /// Smallest and largest palm span seen during calibration.
  double? _minSpan;
  double? _maxSpan;

  List<CameraDevice>? _cameras;
  CameraDevice? _chosen;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_sub != null) return;

    final engine = EngineScope.of(context);
    _sub = engine.frames.listen(_onFrame);
    engine.enumerateCameras().then((c) {
      if (mounted) setState(() => _cameras = c);
    });
  }

  void _onFrame(EngineFrame frame) {
    if (!mounted) return;
    setState(() {
      _frame = frame;

      for (final event in frame.events) {
        if (event.phase == GesturePhase.ended) {
          _gestures.remove(event.trackId);
        } else {
          _gestures[event.trackId] = event.kind;
          if (_step == _Step.gestures) _demonstrated.add(event.kind);
        }
      }
      final live = {for (final h in frame.hands) h.trackId};
      _gestures.removeWhere((trackId, _) => !live.contains(trackId));

      if (_step == _Step.calibrate) _measure(frame);
    });
  }

  /// Records how wide and how narrow the hand gets.
  ///
  /// Measured from the landmarks rather than asked for, because a number a
  /// user types is a number they guessed. The span is wrist-to-middle-tip,
  /// normalized like everything else.
  void _measure(EngineFrame frame) {
    for (final hand in frame.hands) {
      final wrist = hand.landmarks[Lm.wrist];
      final tip = hand.landmarks[Lm.middle + 3];
      final dx = tip.x - wrist.x;
      final dy = tip.y - wrist.y;
      final span = (dx * dx + dy * dy);

      _minSpan = _minSpan == null ? span : (span < _minSpan! ? span : _minSpan);
      _maxSpan = _maxSpan == null ? span : (span > _maxSpan! ? span : _maxSpan);
    }
  }

  /// True when the current step has got what it needs.
  bool get _stepSatisfied => switch (_step) {
        _Step.camera => _cameras != null,
        _Step.calibrate => _calibrationProgress >= 1,
        _Step.gestures => _demonstrated.length >= 3,
        _Step.start => true,
      };

  /// How far through the open-and-close measurement we are, `0..1`.
  double get _calibrationProgress {
    final lo = _minSpan, hi = _maxSpan;
    if (lo == null || hi == null || hi <= 0) return 0;

    // A hand that opened and closed shows a clear spread between its
    // smallest and largest span. Two thirds is a full range.
    final spread = 1 - (lo / hi);
    return (spread / 0.66).clamp(0.0, 1.0);
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  void _go(int delta) {
    final next = _step.index + delta;
    if (next < 0) return;
    if (next >= _Step.values.length) {
      Navigator.of(context).pop();
      return;
    }
    setState(() => _step = _Step.values[next]);
  }

  @override
  Widget build(BuildContext context) {
    final hands = _frame?.hands ?? const <HandObservation>[];

    // Every control here is reachable from the keyboard, and it is said out
    // loud at the bottom of the screen. Setting up a hand-tracking app is
    // exactly when the user may not be able to use their hands — a wizard
    // that can only be completed by gesturing is a door that locks from the
    // inside.
    return Shortcuts(
      shortcuts: const {
        SingleActivator(LogicalKeyboardKey.arrowRight): _NextIntent(),
        SingleActivator(LogicalKeyboardKey.enter): _NextIntent(),
        SingleActivator(LogicalKeyboardKey.arrowLeft): _BackIntent(),
        SingleActivator(LogicalKeyboardKey.escape): _CloseIntent(),
      },
      child: Actions(
        actions: {
          _NextIntent: CallbackAction<_NextIntent>(
            onInvoke: (_) {
              _go(1);
              return null;
            },
          ),
          _BackIntent: CallbackAction<_BackIntent>(
            onInvoke: (_) {
              _go(-1);
              return null;
            },
          ),
          _CloseIntent: CallbackAction<_CloseIntent>(
            onInvoke: (_) {
              Navigator.of(context).pop();
              return null;
            },
          ),
        },
        child: Focus(
          autofocus: true,
          child: Scaffold(
            backgroundColor: HcColors.background,
            body: Padding(
              padding: const EdgeInsets.all(HcSpace.xxl),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  _Steps(current: _step),
                  const SizedBox(height: HcSpace.xl),
                  Text(
                    _step.title,
                    style: Theme.of(context).textTheme.headlineMedium,
                  ),
                  const SizedBox(height: 3),
                  Text(
                    'Step ${_step.index + 1} of ${_Step.values.length}',
                    style: Theme.of(context).textTheme.bodySmall,
                  ),
                  const SizedBox(height: HcSpace.xl),
                  Expanded(
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Expanded(
                          flex: 3,
                          child: PreviewSurface(
                            hands: hands,
                            gestures: _gestures,
                            caption: 'Setup preview',
                          ),
                        ),
                        const SizedBox(width: HcSpace.xl),
                        Expanded(flex: 2, child: _panel()),
                      ],
                    ),
                  ),
                  const SizedBox(height: HcSpace.lg),
                  _Footer(
                    step: _step,
                    canContinue: _stepSatisfied,
                    onBack: () => _go(-1),
                    onNext: () => _go(1),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _panel() => switch (_step) {
        _Step.camera => _CameraStep(
            cameras: _cameras,
            chosen: _chosen,
            onChoose: (c) => setState(() => _chosen = c),
          ),
        _Step.calibrate => _CalibrateStep(
            handInFrame: (_frame?.hands ?? const []).isNotEmpty,
            progress: _calibrationProgress,
          ),
        _Step.gestures => _GesturesStep(demonstrated: _demonstrated),
        _Step.start => const _StartStep(),
      };
}

class _NextIntent extends Intent {
  const _NextIntent();
}

class _BackIntent extends Intent {
  const _BackIntent();
}

class _CloseIntent extends Intent {
  const _CloseIntent();
}

class _Steps extends StatelessWidget {
  const _Steps({required this.current});

  final _Step current;

  @override
  Widget build(BuildContext context) => Row(
        children: [
          for (final step in _Step.values) ...[
            _Dot(step: step, current: current),
            if (step != _Step.values.last)
              Expanded(
                child: Container(
                  height: 1,
                  margin: const EdgeInsets.symmetric(horizontal: HcSpace.md),
                  color: step.index < current.index
                      ? HcColors.accent
                      : HcColors.border,
                ),
              ),
          ],
        ],
      );
}

class _Dot extends StatelessWidget {
  const _Dot({required this.step, required this.current});

  final _Step step;
  final _Step current;

  @override
  Widget build(BuildContext context) {
    final done = step.index < current.index;
    final here = step == current;
    final color = done || here ? HcColors.accent : HcColors.textFaint;

    return Row(
      children: [
        Container(
          width: 20,
          height: 20,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            color: here ? HcColors.accent : Colors.transparent,
            shape: BoxShape.circle,
            border: Border.all(color: color),
          ),
          child: done
              ? const Icon(Icons.check, size: 11, color: HcColors.accent)
              : Text(
                  '${step.index + 1}',
                  style: TextStyle(
                    fontFamily: HcFont.sans,
                    fontSize: 10,
                    fontWeight: FontWeight.w700,
                    color: here ? HcColors.textOnAccent : color,
                  ),
                ),
        ),
        const SizedBox(width: HcSpace.sm),
        Text(
          step.label,
          style: TextStyle(
            fontFamily: HcFont.sans,
            fontSize: 12,
            fontWeight: here ? FontWeight.w600 : FontWeight.w500,
            color: here ? HcColors.text : HcColors.textMuted,
          ),
        ),
      ],
    );
  }
}

class _CameraStep extends StatelessWidget {
  const _CameraStep({
    required this.cameras,
    required this.chosen,
    required this.onChoose,
  });

  final List<CameraDevice>? cameras;
  final CameraDevice? chosen;
  final ValueChanged<CameraDevice> onChoose;

  @override
  Widget build(BuildContext context) {
    final found = cameras;

    return HcPanel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const HcLabel('CAMERA'),
          const SizedBox(height: HcSpace.md),
          if (found == null)
            Text('Looking for cameras',
                style: Theme.of(context).textTheme.bodySmall)
          else if (found.isEmpty)
            Text(
              'No camera was found. You can finish setup and pick one later, '
              'but nothing will be tracked until there is one.',
              style: Theme.of(context).textTheme.bodySmall,
            )
          else
            for (final camera in found)
              Padding(
                padding: const EdgeInsets.only(bottom: HcSpace.sm),
                child: _Choice(
                  label: '${camera.name}\n${camera.path}',
                  selected: camera == (chosen ?? found.first),
                  onTap: () => onChoose(camera),
                ),
              ),
        ],
      ),
    );
  }
}

class _CalibrateStep extends StatelessWidget {
  const _CalibrateStep({required this.handInFrame, required this.progress});

  final bool handInFrame;
  final double progress;

  @override
  Widget build(BuildContext context) => HcPanel(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  handInFrame ? Icons.check_circle_outline : Icons.search,
                  size: 15,
                  color: handInFrame ? HcColors.ok : HcColors.textFaint,
                ),
                const SizedBox(width: HcSpace.md),
                Text(
                  handInFrame ? 'Hand detected' : 'Show your hand',
                  style: Theme.of(context).textTheme.titleSmall,
                ),
              ],
            ),
            const SizedBox(height: HcSpace.lg),
            Text(
              'Open and close your hand slowly a few times. This measures '
              'how far your fingers travel, which is what the pinch and '
              'open-palm thresholds are scaled against.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
            const SizedBox(height: HcSpace.lg),
            const HcLabel('RANGE'),
            const SizedBox(height: HcSpace.sm),
            HcConfidence(
              value: progress,
              color: progress >= 1 ? HcColors.ok : HcColors.accent,
              width: 160,
            ),
            if (progress >= 1) ...[
              const SizedBox(height: HcSpace.lg),
              const HcPill(
                label: 'range measured',
                color: HcColors.ok,
                background: HcColors.okBackground,
              ),
            ],
          ],
        ),
      );
}

class _GesturesStep extends StatelessWidget {
  const _GesturesStep({required this.demonstrated});

  final Set<GestureKind> demonstrated;

  /// The three the default configuration actually needs first.
  static const _toTry = [
    GestureKind.pointIndex,
    GestureKind.twoFingers,
    GestureKind.openPalm,
  ];

  @override
  Widget build(BuildContext context) => HcPanel(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const HcLabel('TRY THESE'),
            const SizedBox(height: HcSpace.md),
            for (final gesture in _toTry)
              Padding(
                padding: const EdgeInsets.only(bottom: HcSpace.md),
                child: Row(
                  children: [
                    Icon(
                      demonstrated.contains(gesture)
                          ? Icons.check_circle_outline
                          : Icons.circle_outlined,
                      size: 15,
                      color: demonstrated.contains(gesture)
                          ? HcColors.ok
                          : HcColors.textFaint,
                    ),
                    const SizedBox(width: HcSpace.md),
                    Expanded(
                      child: Text(gesture.label,
                          style: Theme.of(context).textTheme.bodyMedium),
                    ),
                  ],
                ),
              ),
            const SizedBox(height: HcSpace.sm),
            Text(
              'Nothing is being sent to your desktop yet. Input stays off '
              'until you turn it on in Live View.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ],
        ),
      );
}

class _StartStep extends StatelessWidget {
  const _StartStep();

  @override
  Widget build(BuildContext context) => HcPanel(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const HcLabel('READY'),
            const SizedBox(height: HcSpace.md),
            Text(
              'Point with your index finger to move the cursor, raise the '
              'middle finger to click. The left hand holds the arrow keys.',
              style: Theme.of(context).textTheme.bodyMedium,
            ),
            const SizedBox(height: HcSpace.lg),
            Text(
              'Input is still off. Turn it on from Live View when you want '
              'gestures to reach your desktop — and remember the panic '
              'hotkey in Settings before you do.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ],
        ),
      );
}

class _Footer extends StatelessWidget {
  const _Footer({
    required this.step,
    required this.canContinue,
    required this.onBack,
    required this.onNext,
  });

  final _Step step;
  final bool canContinue;
  final VoidCallback onBack;
  final VoidCallback onNext;

  @override
  Widget build(BuildContext context) => Row(
        children: [
          const Icon(Icons.keyboard_outlined,
              size: 14, color: HcColors.textFaint),
          const SizedBox(width: HcSpace.sm),
          Expanded(
            child: Text(
              "Can't use your hands? Arrow keys move between steps, Enter "
              'continues, Escape closes.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
          HcButton(
            label: 'Back',
            icon: Icons.arrow_back,
            onPressed: step.index == 0 ? null : onBack,
          ),
          const SizedBox(width: HcSpace.sm),
          HcButton(
            label: step == _Step.values.last ? 'Finish' : 'Continue',
            icon: Icons.arrow_forward,
            primary: true,

            // A step that has not got what it needs can still be skipped.
            // The alternative is a wizard that traps a user whose camera is
            // missing, which is exactly who most needs to reach Settings.
            onPressed: onNext,
            tooltip: canContinue ? null : 'You can come back to this later',
          ),
        ],
      );
}

class _Choice extends StatelessWidget {
  const _Choice({
    required this.label,
    required this.selected,
    required this.onTap,
  });

  final String label;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) => Material(
        color: selected ? HcColors.accent : HcColors.surfaceActive,
        borderRadius: BorderRadius.circular(HcRadius.md),
        child: InkWell(
          onTap: onTap,
          borderRadius: BorderRadius.circular(HcRadius.md),
          child: Padding(
            padding: const EdgeInsets.symmetric(
                horizontal: HcSpace.md, vertical: HcSpace.md),
            child: Text(
              label,
              style: TextStyle(
                fontFamily: HcFont.sans,
                fontSize: 12,
                fontWeight: FontWeight.w500,
                color:
                    selected ? HcColors.textOnAccent : HcColors.textMuted,
              ),
            ),
          ),
        ),
      );
}
