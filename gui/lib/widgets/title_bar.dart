import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../theme/tokens.dart';

/// Talks to the GTK runner about the window.
///
/// The window is undecorated, so dragging, minimising, maximising and
/// closing are not free any more — each is an explicit request. The runner
/// side is `linux/runner/window_controls.cc`; there is no plugin and no pub
/// dependency involved.
///
/// Every call is a no-op where the channel is not registered, which is the
/// case in tests and on any platform whose runner has not been taught this
/// yet. A title bar that threw in a widget test would be a poor trade for a
/// window button.
abstract final class WindowControls {
  static const _channel = MethodChannel('dev.handcontroller/window');

  static Future<void> startDrag() => _invoke<void>('startDrag');
  static Future<void> minimize() => _invoke<void>('minimize');
  static Future<void> close() => _invoke<void>('close');

  /// Returns whether the window is maximized AFTER the toggle.
  static Future<bool> toggleMaximize() async =>
      await _invoke<bool>('toggleMaximize') ?? false;

  static Future<bool> isMaximized() async =>
      await _invoke<bool>('isMaximized') ?? false;

  static Future<T?> _invoke<T>(String method) async {
    try {
      return await _channel.invokeMethod<T>(method);
    } on MissingPluginException {
      return null;
    } on PlatformException {
      return null;
    }
  }
}

/// The app's own title bar, replacing the system one.
///
/// Deliberately thin and quiet: the window's identity is already in the
/// sidebar wordmark, so repeating the name here would be the same
/// duplication the GNOME header bar was removed to avoid. What is left is
/// the part a window genuinely needs — somewhere to grab, and the three
/// buttons.
class TitleBar extends StatefulWidget {
  const TitleBar({super.key, this.sidebarWidth = 0});

  static const height = 32.0;

  /// How much of the bar should carry the sidebar's colour.
  ///
  /// The bar spans the window, but the sidebar below it is a different
  /// shade; without this the two meet in a visible seam across the top-left
  /// corner. Matching the first [sidebarWidth] pixels lets the sidebar read
  /// as running the full height of the window.
  final double sidebarWidth;

  @override
  State<TitleBar> createState() => _TitleBarState();
}

class _TitleBarState extends State<TitleBar> {
  var _maximized = false;

  @override
  void initState() {
    super.initState();
    WindowControls.isMaximized().then((v) {
      if (mounted) setState(() => _maximized = v);
    });
  }

  @override
  Widget build(BuildContext context) => SizedBox(
        height: TitleBar.height,
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,

          // Drag starts on pan rather than on tap-down, so a click that was
          // meant for a button underneath is not swallowed by the drag.
          onPanStart: (_) => WindowControls.startDrag(),
          onDoubleTap: () async {
            final now = await WindowControls.toggleMaximize();
            if (mounted) setState(() => _maximized = now);
          },
          child: Row(
            children: [
              if (widget.sidebarWidth > 0)
                Container(
                  width: widget.sidebarWidth,
                  height: TitleBar.height,
                  decoration: const BoxDecoration(
                    color: HcColors.surface,
                    border: Border(
                      right: BorderSide(color: HcColors.border),
                    ),
                  ),
                ),
              const Spacer(),
              _WindowButton(
                icon: Icons.remove,
                tooltip: 'Minimise',
                onPressed: WindowControls.minimize,
              ),
              _WindowButton(
                icon: _maximized
                    ? Icons.filter_none_outlined
                    : Icons.crop_square,
                tooltip: _maximized ? 'Restore' : 'Maximise',
                onPressed: () async {
                  final now = await WindowControls.toggleMaximize();
                  if (mounted) setState(() => _maximized = now);
                },
              ),
              _WindowButton(
                icon: Icons.close,
                tooltip: 'Close',
                hoverColor: HcColors.danger,
                onPressed: WindowControls.close,
              ),
            ],
          ),
        ),
      );
}

class _WindowButton extends StatefulWidget {
  const _WindowButton({
    required this.icon,
    required this.tooltip,
    required this.onPressed,
    this.hoverColor,
  });

  final IconData icon;
  final String tooltip;
  final VoidCallback onPressed;

  /// Close gets a red hover, the convention everywhere else.
  final Color? hoverColor;

  @override
  State<_WindowButton> createState() => _WindowButtonState();
}

class _WindowButtonState extends State<_WindowButton> {
  var _hovered = false;

  @override
  Widget build(BuildContext context) => Tooltip(
        message: widget.tooltip,
        waitDuration: const Duration(milliseconds: 700),
        child: MouseRegion(
          cursor: SystemMouseCursors.click,
          onEnter: (_) => setState(() => _hovered = true),
          onExit: (_) => setState(() => _hovered = false),
          child: GestureDetector(
            onTap: widget.onPressed,
            child: Container(
              width: 44,
              height: TitleBar.height,
              color: _hovered
                  ? (widget.hoverColor ?? HcColors.surfaceActive)
                  : Colors.transparent,
              child: Icon(
                widget.icon,
                size: 14,
                color: _hovered && widget.hoverColor != null
                    ? HcColors.textOnAccent
                    : HcColors.textMuted,
              ),
            ),
          ),
        ),
      );
}
