import 'dart:async';

import 'package:flutter/material.dart';

import '../app.dart';
import '../domain/observation.dart';
import '../engine/engine.dart';
import '../state/app_settings.dart';
import '../theme/tokens.dart';
import '../widgets/common.dart';

/// How bad a finding is.
enum _Severity {
  critical('CRITICAL', HcColors.danger, HcColors.dangerBackground),
  warning('WARNING', HcColors.warn, HcColors.warnBackground),
  ok('OK', HcColors.ok, HcColors.okBackground);

  const _Severity(this.label, this.color, this.background);

  final String label;
  final Color color;
  final Color background;
}

/// One thing that is wrong, or notably right.
class _Finding {
  const _Finding({
    required this.severity,
    required this.title,
    required this.detail,
    this.fixes = const [],
  });

  final _Severity severity;
  final String title;
  final String detail;

  /// What the user can actually do about it. Empty for a healthy card.
  final List<String> fixes;
}

class DiagnosticsScreen extends StatefulWidget {
  const DiagnosticsScreen({super.key});

  @override
  State<DiagnosticsScreen> createState() => _DiagnosticsScreenState();
}

class _DiagnosticsScreenState extends State<DiagnosticsScreen> {
  StreamSubscription<EngineFrame>? _sub;

  /// Null until the first frame and the first enumeration arrive.
  ///
  /// NOT zero and NOT an empty list. "We have not measured yet" and "we
  /// measured and the answer is bad" are different states, and collapsing
  /// them makes this screen accuse the system of a critical fault for the
  /// first few hundred milliseconds after it opens.
  EngineMetrics? _metrics;
  List<CameraDevice>? _cameras;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_sub != null) return;

    final engine = EngineScope.of(context);
    _sub = engine.frames.listen((f) {
      if (mounted) setState(() => _metrics = f.metrics);
    });
    engine.enumerateCameras().then((c) {
      if (mounted) setState(() => _cameras = c);
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final engine = EngineScope.of(context);
    final settings = SettingsScope.of(context);

    return ValueListenableBuilder(
      valueListenable: engine.state,
      builder: (context, state, _) {
        final findings = _assess(state, settings);
        final problems =
            findings.where((f) => f.severity != _Severity.ok).length;

        return Padding(
          padding: const EdgeInsets.all(HcSpace.xxl),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              ScreenHeader(
                title: 'Diagnostics',
                subtitle: problems == 0
                    ? 'Nothing to report'
                    : '$problems issue${problems == 1 ? '' : 's'} detected',
              ),
              Expanded(
                child: ListView(
                  children: [
                    for (final finding in findings)
                      _FindingCard(finding: finding),
                  ],
                ),
              ),
            ],
          ),
        );
      },
    );
  }

  /// Everything worth telling the user, worst first.
  List<_Finding> _assess(EngineState state, AppSettings settings) {
    final findings = <_Finding>[];

    // --- The silent one --------------------------------------------------
    //
    // XTest returns success and does nothing on a Wayland session. Anything
    // that inferred health from "commands were sent" would report this
    // system as working. It is first on the page because every other
    // symptom a user would report — "nothing happens when I point" — leads
    // here.
    if (state.sink == SinkStatus.unavailable) {
      findings.add(_Finding(
        severity: _Severity.critical,
        title: 'Input backend unavailable',
        detail: state.sinkReason ??
            'Commands are being produced and discarded. Nothing you do with '
                'your hands will reach the desktop.',
        fixes: [
          'Log out and choose an X11 session ("GNOME on Xorg" or similar) '
              'at the login screen.',
          'Or install a Wayland input backend and enable its daemon.',
          'Until then, Live View still shows what WOULD be sent.',
        ],
      ));
    } else if (state.sink == SinkStatus.observeOnly) {
      findings.add(const _Finding(
        severity: _Severity.ok,
        title: 'Tracking only',
        detail: 'Input injection is switched off, so commands are shown but '
            'not sent. This is a normal mode, not a fault.',
      ));
    } else {
      findings.add(const _Finding(
        severity: _Severity.ok,
        title: 'Input backend working',
        detail: 'Commands reach the desktop.',
      ));
    }

    // --- Frame rate -------------------------------------------------------
    final metrics = _metrics;
    if (state.lifecycle.isLive && metrics != null && metrics.fps < 15) {
      findings.add(_Finding(
        severity: _Severity.warning,
        title: 'Low frame rate',
        detail: '${metrics.fps.toStringAsFixed(1)} fps. Gestures will feel '
            'late and short ones may be missed entirely.',
        fixes: [
          'Settings -> Camera: drop the resolution to '
              '${CaptureResolution.low.label}.',
          'Settings -> Detection: set the threshold to Low.',
          'Close other CPU-heavy applications.',
        ],
      ));
    }

    // Dropped frames are a separate finding from a low rate. A steady 28 fps
    // with frames being discarded means the pipeline is overrunning its
    // budget in bursts, which a frame-rate average hides completely.
    if (metrics != null && metrics.droppedFrames > 0) {
      findings.add(_Finding(
        severity: _Severity.warning,
        title: 'Frames dropped',
        detail: '${metrics.droppedFrames} frames were skipped because the '
            'pipeline could not keep up. A gesture that started and ended '
            'inside a gap was never seen.',
      ));
    }

    // --- Camera -----------------------------------------------------------
    final cameras = _cameras;
    if (cameras == null) {
      findings.add(const _Finding(
        severity: _Severity.ok,
        title: 'Looking for cameras',
        detail: 'Enumerating video devices.',
      ));
    } else if (cameras.isEmpty) {
      findings.add(const _Finding(
        severity: _Severity.critical,
        title: 'No camera',
        detail: 'No /dev/video* device is present. There is nothing to track.',
        fixes: [
          'Plug in a camera, or check that the kernel module for the '
              'built-in one is loaded.',
        ],
      ));
    } else {
      final camera = state.camera ?? cameras.first;
      findings.add(_Finding(
        severity: _Severity.ok,
        title: 'Camera available',
        detail: '${camera.name} - ${camera.path} at '
            '${settings.resolution.label}',
      ));
    }

    // --- Engine -----------------------------------------------------------
    if (state.lifecycle == EngineLifecycle.failed) {
      findings.add(_Finding(
        severity: _Severity.critical,
        title: 'Engine failed to start',
        detail: state.fault ?? 'No reason was reported.',
      ));
    }

    findings.sort((a, b) => a.severity.index.compareTo(b.severity.index));
    return findings;
  }
}

class _FindingCard extends StatelessWidget {
  const _FindingCard({required this.finding});

  final _Finding finding;

  @override
  Widget build(BuildContext context) {
    final healthy = finding.severity == _Severity.ok;

    return Padding(
      padding: const EdgeInsets.only(bottom: HcSpace.lg),
      child: HcPanel(
        color: healthy ? HcColors.surfaceRaised : finding.severity.background,
        borderColor:
            healthy ? HcColors.border : finding.severity.color.withValues(alpha: 0.3),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  switch (finding.severity) {
                    _Severity.critical => Icons.error_outline,
                    _Severity.warning => Icons.warning_amber_outlined,
                    _Severity.ok => Icons.check_circle_outline,
                  },
                  size: 16,
                  color: finding.severity.color,
                ),
                const SizedBox(width: HcSpace.md),
                Expanded(
                  child: Text(finding.title,
                      style: Theme.of(context).textTheme.titleMedium),
                ),
                HcPill(
                  label: finding.severity.label,
                  color: finding.severity.color,
                ),
              ],
            ),
            const SizedBox(height: HcSpace.md),
            Padding(
              padding: const EdgeInsets.only(left: 28),
              child: Text(finding.detail,
                  style: Theme.of(context).textTheme.bodyMedium),
            ),
            if (finding.fixes.isNotEmpty) ...[
              const SizedBox(height: HcSpace.lg),
              Padding(
                padding: const EdgeInsets.only(left: 28),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const HcLabel('WHAT TO DO'),
                    const SizedBox(height: HcSpace.sm),
                    for (final fix in finding.fixes)
                      Padding(
                        padding: const EdgeInsets.only(bottom: 5),
                        child: Row(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Padding(
                              padding: EdgeInsets.only(top: 6),
                              child: Icon(Icons.circle,
                                  size: 4, color: HcColors.textFaint),
                            ),
                            const SizedBox(width: HcSpace.md),
                            Expanded(
                              child: Text(fix,
                                  style:
                                      Theme.of(context).textTheme.bodySmall),
                            ),
                          ],
                        ),
                      ),
                  ],
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}
