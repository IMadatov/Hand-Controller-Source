import 'package:flutter/material.dart';

import 'domain/observation.dart';
import 'engine/engine.dart';
import 'screens/bindings_screen.dart';
import 'screens/diagnostics_screen.dart';
import 'screens/live_view.dart';
import 'screens/settings_screen.dart';
import 'theme/app_theme.dart';
import 'theme/tokens.dart';
import 'widgets/common.dart';

/// Hands the engine down the tree.
///
/// The type here is the interface, never [MockEngine]. A widget that reaches
/// for the concrete type is a widget that will break when FFI lands, which is
/// the one thing this whole boundary exists to prevent.
class EngineScope extends InheritedWidget {
  const EngineScope({
    super.key,
    required this.engine,
    required super.child,
  });

  final HandControllerEngine engine;

  static HandControllerEngine of(BuildContext context) {
    final scope = context.dependOnInheritedWidgetOfExactType<EngineScope>();
    assert(scope != null, 'No EngineScope above this widget');
    return scope!.engine;
  }

  @override
  bool updateShouldNotify(EngineScope oldWidget) =>
      oldWidget.engine != engine;
}

class HandControllerApp extends StatelessWidget {
  const HandControllerApp({super.key, required this.engine});

  final HandControllerEngine engine;

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'Hand Controller',
        debugShowCheckedModeBanner: false,
        theme: buildHcTheme(),
        home: EngineScope(engine: engine, child: const AppShell()),
      );
}

/// The screens, in the order the sidebar lists them.
enum AppSection {
  live('Live View', Icons.videocam_outlined),
  bindings('Commands', Icons.gesture_outlined),
  settings('Settings', Icons.tune_outlined),
  diagnostics('Diagnostics', Icons.monitor_heart_outlined);

  const AppSection(this.label, this.icon);

  final String label;
  final IconData icon;
}

class AppShell extends StatefulWidget {
  const AppShell({super.key});

  @override
  State<AppShell> createState() => _AppShellState();
}

class _AppShellState extends State<AppShell> {
  var _section = AppSection.live;

  @override
  Widget build(BuildContext context) => Scaffold(
        body: Row(
          children: [
            _Sidebar(
              current: _section,
              onSelect: (s) => setState(() => _section = s),
            ),
            Expanded(
              child: switch (_section) {
                AppSection.live => const LiveViewScreen(),
                AppSection.bindings => const BindingsScreen(),
                AppSection.settings => const SettingsScreen(),
                AppSection.diagnostics => const DiagnosticsScreen(),
              },
            ),
          ],
        ),
      );
}

class _Sidebar extends StatelessWidget {
  const _Sidebar({required this.current, required this.onSelect});

  final AppSection current;
  final ValueChanged<AppSection> onSelect;

  @override
  Widget build(BuildContext context) => Container(
        width: 208,
        decoration: const BoxDecoration(
          color: HcColors.surface,
          border: Border(right: BorderSide(color: HcColors.border)),
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            const _Wordmark(),
            const Divider(height: 1),
            const SizedBox(height: HcSpace.md),
            for (final section in AppSection.values)
              _NavItem(
                section: section,
                selected: section == current,
                onTap: () => onSelect(section),
              ),
            const Spacer(),
            const _SinkFooter(),
          ],
        ),
      );
}

class _Wordmark extends StatelessWidget {
  const _Wordmark();

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.all(HcSpace.lg),
        child: Row(
          children: [
            Container(
              width: 26,
              height: 26,
              alignment: Alignment.center,
              decoration: BoxDecoration(
                gradient: const LinearGradient(
                  colors: [HcColors.accent, HcColors.accentBright],
                ),
                borderRadius: BorderRadius.circular(HcRadius.md),
              ),
              child: const Icon(Icons.back_hand_outlined,
                  size: 15, color: HcColors.textOnAccent),
            ),
            const SizedBox(width: HcSpace.md),
            const Text(
              'Hand Controller',
              style: TextStyle(
                fontFamily: HcFont.display,
                fontSize: 14,
                fontWeight: FontWeight.w600,
                color: HcColors.text,
              ),
            ),
          ],
        ),
      );
}

class _NavItem extends StatelessWidget {
  const _NavItem({
    required this.section,
    required this.selected,
    required this.onTap,
  });

  final AppSection section;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.symmetric(
            horizontal: HcSpace.md, vertical: 1),
        child: Material(
          color: selected ? HcColors.surfaceActive : Colors.transparent,
          borderRadius: BorderRadius.circular(HcRadius.md),
          child: InkWell(
            onTap: onTap,
            borderRadius: BorderRadius.circular(HcRadius.md),
            child: Padding(
              padding: const EdgeInsets.symmetric(
                  horizontal: HcSpace.md, vertical: HcSpace.md),
              child: Row(
                children: [
                  Icon(
                    section.icon,
                    size: 16,
                    color: selected ? HcColors.accent : HcColors.textFaint,
                  ),
                  const SizedBox(width: HcSpace.md),
                  Text(
                    section.label,
                    style: TextStyle(
                      fontFamily: HcFont.sans,
                      fontSize: 13,
                      fontWeight: selected ? FontWeight.w600 : FontWeight.w500,
                      color: selected ? HcColors.text : HcColors.textMuted,
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      );
}

/// Whether input is really being injected, visible from every screen.
///
/// It lives in the chrome rather than on one page because the answer changes
/// what every page means: in observe-only mode the command log is a preview,
/// not a record of things that happened.
class _SinkFooter extends StatelessWidget {
  const _SinkFooter();

  @override
  Widget build(BuildContext context) {
    final engine = EngineScope.of(context);

    return ValueListenableBuilder(
      valueListenable: engine.state,
      builder: (context, state, _) {
        final (color, icon) = switch (state.sink) {
          SinkStatus.active => (HcColors.ok, Icons.check_circle_outline),
          SinkStatus.observeOnly => (HcColors.textMuted, Icons.visibility_outlined),
          SinkStatus.unavailable => (HcColors.danger, Icons.error_outline),
        };

        return Padding(
          padding: const EdgeInsets.all(HcSpace.md),
          child: Tooltip(
            message: state.sinkReason ?? state.sink.label,
            child: HcPanel(
              color: HcColors.surfaceRaised,
              padding: const EdgeInsets.symmetric(
                  horizontal: HcSpace.md, vertical: HcSpace.sm),
              child: Row(
                children: [
                  Icon(icon, size: 13, color: color),
                  const SizedBox(width: HcSpace.sm),
                  Expanded(
                    child: Text(
                      state.sink.label,
                      style: TextStyle(
                        fontFamily: HcFont.sans,
                        fontSize: 11,
                        fontWeight: FontWeight.w500,
                        color: color,
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ),
        );
      },
    );
  }
}

/// A screen's heading row.
class ScreenHeader extends StatelessWidget {
  const ScreenHeader({
    super.key,
    required this.title,
    this.subtitle,
    this.trailing,
  });

  final String title;
  final String? subtitle;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.only(bottom: HcSpace.xl),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.center,
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(title,
                      style: Theme.of(context).textTheme.headlineMedium),
                  if (subtitle != null) ...[
                    const SizedBox(height: 3),
                    Text(subtitle!,
                        style: Theme.of(context).textTheme.bodySmall),
                  ],
                ],
              ),
            ),
            ?trailing,
          ],
        ),
      );
}
