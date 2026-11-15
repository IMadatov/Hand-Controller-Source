import 'dart:async';

import 'package:flutter/material.dart';

import '../app.dart';
import '../domain/gesture.dart';
import '../domain/observation.dart';
import '../engine/engine.dart';
import '../theme/tokens.dart';
import '../widgets/common.dart';

/// The compact heads-up display, 320x220.
///
/// It is meant to live in a small always-on-top window beside whatever the
/// user is actually doing — the whole point is that you do not have to look
/// away from your work to see whether the app understood your hand.
///
/// **It is not in its own window yet.** A second native window is a
/// platform-layer change, and the rest of this app does not need one. For now
/// it is shown inside the main window so the layout can be built and
/// reviewed; moving it out is a contained change because nothing here knows
/// where it is drawn.
class TrayOverlay extends StatefulWidget {
  const TrayOverlay({super.key});

  /// The size the real window will be.
  static const size = Size(320, 220);

  @override
  State<TrayOverlay> createState() => _TrayOverlayState();
}

class _TrayOverlayState extends State<TrayOverlay> {
  StreamSubscription<EngineFrame>? _sub;

  EngineFrame? _frame;
  final _gestures = <int, GestureKind>{};
  CommandRecord? _lastCommand;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_sub != null) return;
    _sub = EngineScope.of(context).frames.listen(_onFrame);
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
        }
      }
      final live = {for (final h in frame.hands) h.trackId};
      _gestures.removeWhere((trackId, _) => !live.contains(trackId));

      if (frame.commands.isNotEmpty) _lastCommand = frame.commands.last;
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  HandObservation? _hand(Handedness side) {
    for (final h in _frame?.hands ?? const <HandObservation>[]) {
      if (h.handedness == side) return h;
    }
    return null;
  }

  @override
  Widget build(BuildContext context) {
    final engine = EngineScope.of(context);

    return ValueListenableBuilder(
      valueListenable: engine.state,
      builder: (context, state, _) => Container(
        width: TrayOverlay.size.width,
        height: TrayOverlay.size.height,
        decoration: BoxDecoration(
          color: HcColors.background,
          borderRadius: BorderRadius.circular(HcRadius.lg),
          border: Border.all(color: HcColors.border),
        ),
        padding: const EdgeInsets.all(HcSpace.md),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            _Header(state: state),
            const SizedBox(height: HcSpace.md),
            _HandLine(
              hand: _hand(Handedness.right),
              gestures: _gestures,
              fallbackLabel: 'Right',
            ),
            const SizedBox(height: 5),
            _HandLine(
              hand: _hand(Handedness.left),
              gestures: _gestures,
              fallbackLabel: 'Left',
            ),
            const SizedBox(height: HcSpace.sm),
            _LastCommand(
              record: _lastCommand,
              fps: _frame?.metrics.fps ?? 0,
              live: state.lifecycle.isLive,
            ),
            const Spacer(),
            _Actions(state: state),
          ],
        ),
      ),
    );
  }
}

class _Header extends StatelessWidget {
  const _Header({required this.state});

  final EngineState state;

  @override
  Widget build(BuildContext context) {
    final (color, background) = switch (state.lifecycle) {
      EngineLifecycle.running => (HcColors.ok, HcColors.okBackground),
      EngineLifecycle.paused => (HcColors.warn, HcColors.warnBackground),
      _ => (HcColors.textMuted, HcColors.surfaceActive),
    };

    return Row(
      children: [
        Container(
          width: 18,
          height: 18,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            gradient: const LinearGradient(
              colors: [HcColors.accent, HcColors.accentBright],
            ),
            borderRadius: BorderRadius.circular(HcRadius.sm + 1),
          ),
          child: const Icon(Icons.back_hand_outlined,
              size: 11, color: HcColors.textOnAccent),
        ),
        const SizedBox(width: HcSpace.sm),
        const Text(
          'Hand Controller',
          style: TextStyle(
            fontFamily: HcFont.display,
            fontSize: 12,
            fontWeight: FontWeight.w600,
            color: HcColors.text,
          ),
        ),
        const Spacer(),

        // The sink state is carried here too. The overlay is the only thing
        // on screen when the user is working, so if it did not say that
        // input is going nowhere, nothing would.
        if (state.sink == SinkStatus.unavailable)
          const HcPill(
            label: 'no input',
            color: HcColors.danger,
            background: HcColors.dangerBackground,
          )
        else
          HcPill(
            label: state.lifecycle.label,
            color: color,
            background: background,
          ),
      ],
    );
  }
}

/// One hand: badge, pose, and how sure the label is.
class _HandLine extends StatelessWidget {
  const _HandLine({
    required this.hand,
    required this.gestures,
    required this.fallbackLabel,
  });

  final HandObservation? hand;
  final Map<int, GestureKind> gestures;
  final String fallbackLabel;

  @override
  Widget build(BuildContext context) {
    final h = hand;
    final gesture = h == null ? null : gestures[h.trackId];

    return DecoratedBox(
      decoration: BoxDecoration(
        color: HcColors.surfaceRaised,
        borderRadius: BorderRadius.circular(HcRadius.md),
        border: Border.all(color: HcColors.border),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(
            horizontal: HcSpace.sm, vertical: 6),
        child: Row(
          children: [
            HandBadge(h?.handedness ?? Handedness.unknown, size: 18),
            const SizedBox(width: HcSpace.sm),
            Expanded(
              child: Text(
                gesture?.shortLabel ??
                    (h == null ? 'Not in frame' : 'No gesture'),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: TextStyle(
                  fontFamily: HcFont.sans,
                  fontSize: 11,
                  fontWeight: FontWeight.w600,
                  color: gesture == null ? HcColors.textFaint : HcColors.text,
                ),
              ),
            ),
            if (h != null)
              Tooltip(
                message: 'Confidence that this is the $fallbackLabel hand',
                child: HcConfidence(
                  value: h.handednessScore,
                  color: h.handednessScore < 0.65
                      ? HcColors.warn
                      : handColor(h.handedness),
                  width: 34,
                ),
              ),
          ],
        ),
      ),
    );
  }
}

class _LastCommand extends StatelessWidget {
  const _LastCommand({
    required this.record,
    required this.fps,
    required this.live,
  });

  final CommandRecord? record;
  final double fps;
  final bool live;

  @override
  Widget build(BuildContext context) => DecoratedBox(
        decoration: BoxDecoration(
          color: HcColors.canvas,
          borderRadius: BorderRadius.circular(HcRadius.md),
          border: Border.all(color: HcColors.surfaceRaised),
        ),
        child: Padding(
          padding: const EdgeInsets.symmetric(
              horizontal: HcSpace.md, vertical: 6),
          child: Row(
            children: [
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const HcLabel('LAST COMMAND'),
                    Text(
                      record?.command.label ?? 'Nothing yet',
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                        fontFamily: HcFont.sans,
                        fontSize: 11,
                        fontWeight: FontWeight.w500,

                        // Dimmed when it was produced but not delivered, so
                        // the overlay never implies something happened on
                        // the desktop that did not.
                        color: (record?.delivered ?? false)
                            ? HcColors.text
                            : HcColors.textMuted,
                      ),
                    ),
                  ],
                ),
              ),
              if (live)
                HcPill(
                  label: '${fps.toStringAsFixed(0)} fps',
                  color: HcColors.ok,
                  background: HcColors.okBackground,
                ),
            ],
          ),
        ),
      );
}

class _Actions extends StatelessWidget {
  const _Actions({required this.state});

  final EngineState state;

  @override
  Widget build(BuildContext context) {
    final engine = EngineScope.of(context);
    final paused = state.lifecycle == EngineLifecycle.paused;

    return Row(
      children: [
        Expanded(
          child: HcButton(
            label: paused ? 'Resume' : 'Pause',
            icon: paused ? Icons.play_arrow : Icons.pause,
            onPressed: state.lifecycle.isLive
                ? () => engine.setPaused(!paused)
                : () => engine.start(),
          ),
        ),
        const SizedBox(width: HcSpace.sm),

        // Reachable without aiming: the cursor may be being driven away from
        // it, which is the whole reason a panic control exists.
        HcButton(
          label: '',
          icon: Icons.stop,
          danger: true,
          tooltip: 'Stop everything',
          onPressed: state.lifecycle.isLive ? engine.stop : null,
        ),
      ],
    );
  }
}
