import 'dart:async';

import 'package:flutter/material.dart';

import '../app.dart';
import '../domain/command.dart';
import '../domain/gesture.dart';
import '../domain/observation.dart';
import '../engine/engine.dart';
import '../theme/app_theme.dart';
import '../theme/tokens.dart';
import '../widgets/common.dart';
import '../widgets/hand_overlay.dart';

class LiveViewScreen extends StatefulWidget {
  const LiveViewScreen({super.key});

  @override
  State<LiveViewScreen> createState() => _LiveViewScreenState();
}

class _LiveViewScreenState extends State<LiveViewScreen> {
  StreamSubscription<EngineFrame>? _frameSub;

  EngineFrame? _frame;

  /// What each track is holding right now, rebuilt from gesture EVENTS.
  ///
  /// The observations do not carry a gesture — the recognizer's output is
  /// the event stream — so "current gesture" is state the UI keeps, not a
  /// field it reads. A track is absent here whenever it holds nothing,
  /// including the one-to-three-frame window between a takeover's `ended`
  /// and the next `began`. That gap is the truth and is rendered as no
  /// gesture rather than smoothed over.
  final _gestures = <int, GestureKind>{};
  final _log = <_LogEntry>[];
  /// Null while enumeration is still in flight. An empty list means the
  /// machine genuinely has no camera, and the two must not look alike.
  List<CameraDevice>? _cameras;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_frameSub != null) return;

    final engine = EngineScope.of(context);
    _frameSub = engine.frames.listen(_onFrame);
    engine.enumerateCameras().then((c) {
      if (mounted) setState(() => _cameras = c);
    });
  }

  void _onFrame(EngineFrame frame) {
    if (!mounted) return;
    setState(() {
      _frame = frame;

      for (final event in frame.events) {
        switch (event.phase) {
          case GesturePhase.began:
          case GesturePhase.held:
            _gestures[event.trackId] = event.kind;
          case GesturePhase.ended:
            _gestures.remove(event.trackId);
        }
      }

      // A track that stopped being reported holds nothing, whatever it was
      // doing last. Without this a hand leaving the frame would keep its
      // label on screen forever.
      final live = {for (final h in frame.hands) h.trackId};
      _gestures.removeWhere((trackId, _) => !live.contains(trackId));

      for (final command in frame.commands) {
        _record(command);
      }
    });
  }

  /// Appends a command to the log, folding repeats.
  ///
  /// A `held` binding fires on EVERY frame — a cursor move produces ~28
  /// entries a second — so an honest log of every command is unreadable
  /// within a second. The stream stays faithful and the view collapses runs,
  /// showing a count, so nothing is hidden and the list stays legible.
  void _record(CommandRecord record) {
    final last = _log.firstOrNull;
    if (last != null && last.foldsWith(record)) {
      last.repeats++;
      last.lastAt = record.at;
    } else {
      _log.insert(0, _LogEntry(record));
    }
    if (_log.length > 60) _log.removeRange(60, _log.length);
  }

  @override
  void dispose() {
    _frameSub?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final engine = EngineScope.of(context);
    final hands = _frame?.hands ?? const <HandObservation>[];
    final metrics = _frame?.metrics ?? EngineMetrics.zero;

    return ValueListenableBuilder(
      valueListenable: engine.state,
      builder: (context, state, _) => Padding(
        padding: const EdgeInsets.all(HcSpace.xxl),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            ScreenHeader(
              title: 'Live View',
              subtitle: state.lifecycle == EngineLifecycle.running
                  ? 'Tracking ${hands.length} hand${hands.length == 1 ? '' : 's'}'
                  : 'The engine is not running',
              trailing: _Controls(state: state, metrics: metrics),
            ),
            Expanded(
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Expanded(
                    flex: 3,
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        PreviewSurface(
                          hands: hands,
                          gestures: _gestures,
                          caption: '1280x720 - HD',
                          badge: state.lifecycle == EngineLifecycle.running
                              ? const HcPill(
                                  label: 'LIVE',
                                  color: HcColors.danger,
                                  icon: Icons.circle,
                                )
                              : null,
                        ),
                        const SizedBox(height: HcSpace.lg),
                        _CameraRow(cameras: _cameras, state: state),
                        const SizedBox(height: HcSpace.lg),
                        Expanded(
                          child: _PipelinePanel(
                            metrics: metrics,
                            state: state,
                          ),
                        ),
                      ],
                    ),
                  ),
                  const SizedBox(width: HcSpace.xl),
                  Expanded(
                    flex: 2,
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        _HandCard(
                          hand: _handFor(hands, Handedness.right),
                          gestures: _gestures,
                        ),
                        const SizedBox(height: HcSpace.md),
                        _HandCard(
                          hand: _handFor(hands, Handedness.left),
                          gestures: _gestures,
                        ),
                        const SizedBox(height: HcSpace.md),
                        Expanded(
                          child: _CommandLog(
                            entries: _log,
                            delivering: state.sink == SinkStatus.active,
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  /// The hand to show in a given slot.
  ///
  /// An unlabelled hand is shown in whichever slot is otherwise empty rather
  /// than dropped. The model not knowing which hand it is does not make the
  /// hand less real, and a user seeing nothing would conclude tracking had
  /// failed.
  HandObservation? _handFor(List<HandObservation> hands, Handedness side) {
    for (final h in hands) {
      if (h.handedness == side) return h;
    }
    final labelled = hands.where((h) => h.handedness != Handedness.unknown);
    final unknown = hands.where((h) => h.handedness == Handedness.unknown);
    if (unknown.isEmpty) return null;
    return labelled.any((h) => h.handedness == side) ? null : unknown.first;
  }
}

/// Start/stop, pause, and the injection switch.
class _Controls extends StatelessWidget {
  const _Controls({required this.state, required this.metrics});

  final EngineState state;
  final EngineMetrics metrics;

  @override
  Widget build(BuildContext context) {
    final engine = EngineScope.of(context);
    final live = state.lifecycle.isLive;
    final paused = state.lifecycle == EngineLifecycle.paused;

    final (pillColor, pillBackground) = switch (state.lifecycle) {
      EngineLifecycle.running => (HcColors.ok, HcColors.okBackground),
      EngineLifecycle.paused => (HcColors.warn, HcColors.warnBackground),
      EngineLifecycle.failed => (HcColors.danger, HcColors.dangerBackground),
      _ => (HcColors.textMuted, HcColors.surfaceActive),
    };

    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        HcPill(
          label: state.lifecycle.label,
          color: pillColor,
          background: pillBackground,
        ),
        if (live) ...[
          const SizedBox(width: HcSpace.md),
          Text(
            '${metrics.fps.toStringAsFixed(1)} fps  '
            '${metrics.latencyMs.toStringAsFixed(0)} ms',
            style: hcMetricStyle.copyWith(color: HcColors.textMuted),
          ),
        ],
        const SizedBox(width: HcSpace.lg),

        // Injection is opt-in, and off on a fresh start. A gesture app that
        // begins by seizing the cursor gives the user no chance to find out
        // what their hands do first.
        _InjectSwitch(state: state),
        const SizedBox(width: HcSpace.lg),

        if (live)
          HcButton(
            label: paused ? 'Resume' : 'Pause',
            icon: paused ? Icons.play_arrow : Icons.pause,
            onPressed: () => engine.setPaused(!paused),
          )
        else
          HcButton(
            label: 'Start',
            icon: Icons.play_arrow,
            primary: true,
            onPressed: state.lifecycle == EngineLifecycle.starting
                ? null
                : () => engine.start(),
          ),
        const SizedBox(width: HcSpace.sm),
        HcButton(
          label: '',
          icon: Icons.stop,
          danger: true,
          tooltip: 'Stop everything (Esc)',
          onPressed: live ? engine.stop : null,
        ),
      ],
    );
  }
}

class _InjectSwitch extends StatelessWidget {
  const _InjectSwitch({required this.state});

  final EngineState state;

  @override
  Widget build(BuildContext context) {
    final engine = EngineScope.of(context);
    final on = state.sink != SinkStatus.observeOnly;
    final failed = state.sink == SinkStatus.unavailable;

    return Tooltip(
      message: state.sinkReason ??
          (on
              ? 'Gestures move the real cursor'
              : 'Gestures are shown but nothing is sent'),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Text(
            'Send input',
            style: TextStyle(
              fontFamily: HcFont.sans,
              fontSize: 12,
              fontWeight: FontWeight.w500,
              color: failed ? HcColors.danger : HcColors.textMuted,
            ),
          ),
          const SizedBox(width: HcSpace.xs),
          Switch(
            value: on,
            onChanged: (v) => engine.setInjectInput(v),
          ),

          // Asking to inject and succeeding are different things, and XTest
          // fails silently. The switch being on is not evidence.
          if (failed)
            const Padding(
              padding: EdgeInsets.only(left: HcSpace.xs),
              child: Icon(Icons.error_outline,
                  size: 15, color: HcColors.danger),
            ),
        ],
      ),
    );
  }
}

class _CameraRow extends StatelessWidget {
  const _CameraRow({required this.cameras, required this.state});

  /// Null while still being enumerated.
  final List<CameraDevice>? cameras;
  final EngineState state;

  @override
  Widget build(BuildContext context) {
    final found = cameras;
    if (found == null) {
      return const HcPanel(
        padding: EdgeInsets.symmetric(
            horizontal: HcSpace.lg, vertical: HcSpace.md),
        child: Row(
          children: [
            Icon(Icons.search, size: 15, color: HcColors.textFaint),
            SizedBox(width: HcSpace.md),
            Text('Looking for cameras', style: TextStyle(
              fontFamily: HcFont.sans,
              fontSize: 12,
              color: HcColors.textFaint,
            )),
          ],
        ),
      );
    }

    if (found.isEmpty) {
      // The build machine has no `/dev/video*` at all, so this is not a rare
      // path and must read as a plain fact rather than a crash.
      return const HcPanel(
        padding: EdgeInsets.symmetric(
            horizontal: HcSpace.lg, vertical: HcSpace.md),
        child: Row(
          children: [
            Icon(Icons.videocam_off_outlined,
                size: 15, color: HcColors.textFaint),
            SizedBox(width: HcSpace.md),
            Text('No camera found', style: TextStyle(
              fontFamily: HcFont.sans,
              fontSize: 12,
              color: HcColors.textMuted,
            )),
          ],
        ),
      );
    }

    final selected = state.camera ?? found.first;

    return HcPanel(
      padding: const EdgeInsets.symmetric(
          horizontal: HcSpace.lg, vertical: HcSpace.sm),
      child: Row(
        children: [
          const Icon(Icons.videocam_outlined,
              size: 15, color: HcColors.textFaint),
          const SizedBox(width: HcSpace.md),
          Expanded(
            child: DropdownButtonHideUnderline(
              child: DropdownButton<CameraDevice>(
                value: selected,
                isDense: true,
                isExpanded: true,
                dropdownColor: HcColors.surfaceRaised,
                borderRadius: BorderRadius.circular(HcRadius.md),
                icon: const Icon(Icons.expand_more,
                    size: 16, color: HcColors.textFaint),
                items: [
                  for (final camera in found)
                    DropdownMenuItem(
                      value: camera,
                      child: Text(
                        '${camera.name} - ${camera.path}',
                        style: const TextStyle(
                          fontFamily: HcFont.sans,
                          fontSize: 12,
                          color: HcColors.text,
                        ),
                      ),
                    ),
                ],
                onChanged: (camera) {
                  if (camera == null) return;
                  final engine = EngineScope.of(context);
                  engine.start(camera: camera);
                },
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// What the pipeline costs, and whether anything is being thrown away.
///
/// Dropped frames are shown next to the frame rate rather than hidden: a
/// smooth-looking 10 fps with half the frames discarded is a different
/// problem from an honest 10 fps, and the fix for each is different.
class _PipelinePanel extends StatelessWidget {
  const _PipelinePanel({required this.metrics, required this.state});

  final EngineMetrics metrics;
  final EngineState state;

  @override
  Widget build(BuildContext context) {
    final live = state.lifecycle.isLive;
    final slow = live && metrics.fps < 15;

    return HcPanel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const HcLabel('PIPELINE'),
              const Spacer(),
              if (slow)
                const HcPill(
                  label: 'below 15 fps',
                  color: HcColors.warn,
                  background: HcColors.warnBackground,
                ),
            ],
          ),
          const SizedBox(height: HcSpace.lg),
          Row(
            children: [
              Expanded(
                child: HcStat(
                  label: 'FRAME RATE',
                  mono: true,
                  value: live
                      ? '${metrics.fps.toStringAsFixed(1)} fps'
                      : '--',
                  valueColor: slow ? HcColors.warn : HcColors.text,
                ),
              ),
              Expanded(
                child: HcStat(
                  label: 'LATENCY',
                  mono: true,
                  value: live
                      ? '${metrics.latencyMs.toStringAsFixed(1)} ms'
                      : '--',
                ),
              ),
              Expanded(
                child: HcStat(
                  label: 'DROPPED',
                  mono: true,
                  value: live ? '${metrics.droppedFrames}' : '--',
                  valueColor: metrics.droppedFrames > 0
                      ? HcColors.warn
                      : HcColors.text,
                ),
              ),
            ],
          ),
          const Spacer(),

          // The one line on this screen that says whether gestures are
          // reaching the desktop. It is spelled out rather than implied,
          // because the failure it describes is invisible by nature.
          Row(
            children: [
              Icon(
                switch (state.sink) {
                  SinkStatus.active => Icons.check_circle_outline,
                  SinkStatus.observeOnly => Icons.visibility_outlined,
                  SinkStatus.unavailable => Icons.error_outline,
                },
                size: 14,
                color: switch (state.sink) {
                  SinkStatus.active => HcColors.ok,
                  SinkStatus.observeOnly => HcColors.textMuted,
                  SinkStatus.unavailable => HcColors.danger,
                },
              ),
              const SizedBox(width: HcSpace.md),
              Expanded(
                child: Text(
                  state.sinkReason ?? state.sink.label,
                  style: Theme.of(context).textTheme.bodySmall,
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

/// One hand's current state, or an empty slot.
class _HandCard extends StatelessWidget {
  const _HandCard({required this.hand, required this.gestures});

  final HandObservation? hand;

  /// What each track holds, from the event stream.
  final Map<int, GestureKind> gestures;

  @override
  Widget build(BuildContext context) {
    final h = hand;
    if (h == null) {
      // Absence of a hand is a NORMAL state, not an error. Rendered quietly.
      return const HcPanel(
        child: Row(
          children: [
            Icon(Icons.pan_tool_outlined, size: 16, color: HcColors.textFaint),
            SizedBox(width: HcSpace.md),
            Text('No hand in frame', style: TextStyle(
              fontFamily: HcFont.sans,
              fontSize: 12,
              color: HcColors.textFaint,
            )),
          ],
        ),
      );
    }

    final color = handColor(h.handedness);
    final weak = h.handednessScore < 0.65;
    final gesture = gestures[h.trackId];

    return HcPanel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              HandBadge(h.handedness),
              const SizedBox(width: HcSpace.md),
              Text(
                '${h.handedness.label} hand',
                style: Theme.of(context).textTheme.titleSmall,
              ),
              const Spacer(),

              // A weak handedness score is called out, because it is the
              // reason a left-hand binding may not fire for what looks to
              // the user like a left hand.
              if (weak)
                HcPill(
                  label: 'low confidence',
                  color: HcColors.warn,
                  background: HcColors.warnBackground,
                )
              else
                HcPill(label: 'tracked', color: color),
            ],
          ),
          const SizedBox(height: HcSpace.lg),
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Expanded(
                flex: 3,
                child: HcStat(
                  label: 'GESTURE',
                  value: gesture?.label ?? 'None',
                  valueColor:
                      gesture == null ? HcColors.textFaint : HcColors.text,
                ),
              ),
              Expanded(
                flex: 2,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const HcLabel('HAND LABEL'),
                    const SizedBox(height: 5),
                    HcConfidence(
                      value: h.handednessScore,
                      color: weak ? HcColors.warn : color,
                    ),
                  ],
                ),
              ),
              Expanded(
                flex: 2,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    // Presence, not a gesture score. The observation says
                    // how sure it is that this is a HAND; how sure it is
                    // about the POSE is the recognizer's business and
                    // reaches the UI as an event or not at all.
                    const HcLabel('PRESENCE'),
                    const SizedBox(height: 5),
                    HcConfidence(
                      value: h.presence,
                      color: HcColors.accent,
                    ),
                  ],
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

/// A log row, with a run of identical continuous commands folded into it.
class _LogEntry {
  _LogEntry(this.record) : lastAt = record.at;

  final CommandRecord record;
  DateTime lastAt;
  int repeats = 1;

  /// Only continuous commands fold. A click must never be folded: two clicks
  /// are not one click, and collapsing them would hide a double fire.
  bool foldsWith(CommandRecord other) {
    final continuous = record.command.kind == CommandKind.moveCursor ||
        record.command.kind == CommandKind.scroll;
    return continuous &&
        other.command.kind == record.command.kind &&
        other.handedness == record.handedness &&
        other.gesture == record.gesture;
  }
}

class _CommandLog extends StatelessWidget {
  const _CommandLog({required this.entries, required this.delivering});

  final List<_LogEntry> entries;

  /// False in observe-only mode, where these commands were produced and
  /// deliberately not sent.
  final bool delivering;

  @override
  Widget build(BuildContext context) => HcPanel(
        padding: const EdgeInsets.all(HcSpace.md),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(
                  HcSpace.xs, HcSpace.xs, HcSpace.xs, HcSpace.md),
              child: Row(
                children: [
                  const HcLabel('COMMAND LOG'),
                  const Spacer(),
                  if (!delivering)
                    const HcPill(
                      label: 'not sent',
                      color: HcColors.textMuted,
                    ),
                ],
              ),
            ),
            Expanded(
              child: entries.isEmpty
                  ? const Center(
                      child: Text(
                        'Nothing yet',
                        style: TextStyle(
                          fontFamily: HcFont.sans,
                          fontSize: 12,
                          color: HcColors.textFaint,
                        ),
                      ),
                    )
                  : ListView.builder(
                      padding: EdgeInsets.zero,
                      itemCount: entries.length,
                      itemBuilder: (context, i) => _LogRow(entry: entries[i]),
                    ),
            ),
          ],
        ),
      );
}

class _LogRow extends StatelessWidget {
  const _LogRow({required this.entry});

  final _LogEntry entry;

  @override
  Widget build(BuildContext context) {
    final record = entry.record;
    final color = handColor(record.handedness);
    final at = entry.lastAt;
    final clock = '${at.hour.toString().padLeft(2, '0')}:'
        '${at.minute.toString().padLeft(2, '0')}:'
        '${at.second.toString().padLeft(2, '0')}';

    return Padding(
      padding: const EdgeInsets.symmetric(
          horizontal: HcSpace.xs, vertical: 3),
      child: Row(
        children: [
          Text(
            clock,
            style: const TextStyle(
              fontFamily: HcFont.mono,
              fontSize: 10,
              color: HcColors.textFaint,
              fontFeatures: [FontFeature.tabularFigures()],
            ),
          ),
          const SizedBox(width: HcSpace.md),
          HandBadge(record.handedness, size: 16),
          const SizedBox(width: HcSpace.md),
          Expanded(
            child: Text(
              record.command.label,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: TextStyle(
                fontFamily: HcFont.sans,
                fontSize: 12,
                fontWeight: FontWeight.w500,
                color: record.delivered ? HcColors.text : HcColors.textMuted,
              ),
            ),
          ),
          if (entry.repeats > 1)
            Text(
              'x${entry.repeats}',
              style: TextStyle(
                fontFamily: HcFont.mono,
                fontSize: 10,
                fontWeight: FontWeight.w600,
                color: color,
              ),
            ),
        ],
      ),
    );
  }
}
