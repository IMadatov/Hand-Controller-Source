import 'package:flutter/material.dart';

import 'domain/observation.dart';
import 'engine/engine.dart';
import 'screens/bindings_screen.dart';
import 'screens/diagnostics_screen.dart';
import 'screens/live_view.dart';
import 'screens/settings_screen.dart';
import 'state/app_settings.dart';
import 'state/bindings_controller.dart';
import 'theme/app_theme.dart';
import 'theme/tokens.dart';
import 'widgets/common.dart';
import 'widgets/title_bar.dart';

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

/// Hands the working [BindingSet] down the tree.
///
/// Above the screens rather than inside one, because Commands and Settings
/// edit the same configuration: the sensitivity and smoothing sliders are
/// fields of the binding set, not separate preferences.
class BindingsScope extends InheritedNotifier<BindingsController> {
  const BindingsScope({
    super.key,
    required BindingsController controller,
    required super.child,
  }) : super(notifier: controller);

  static BindingsController of(BuildContext context) {
    final scope =
        context.dependOnInheritedWidgetOfExactType<BindingsScope>();
    assert(scope != null, 'No BindingsScope above this widget');
    return scope!.notifier!;
  }
}

/// Hands the app's own preferences down the tree.
class SettingsScope extends InheritedNotifier<AppSettings> {
  const SettingsScope({
    super.key,
    required AppSettings settings,
    required super.child,
  }) : super(notifier: settings);

  static AppSettings of(BuildContext context) {
    final scope = context.dependOnInheritedWidgetOfExactType<SettingsScope>();
    assert(scope != null, 'No SettingsScope above this widget');
    return scope!.notifier!;
  }
}

class HandControllerApp extends StatefulWidget {
  const HandControllerApp({super.key, required this.engine});

  final HandControllerEngine engine;

  @override
  State<HandControllerApp> createState() => _HandControllerAppState();
}

class _HandControllerAppState extends State<HandControllerApp> {
  late final _bindings = BindingsController(widget.engine)..load();
  final _settings = AppSettings();

  @override
  void dispose() {
    _bindings.dispose();
    _settings.dispose();
    super.dispose();
  }

  /// The scopes sit ABOVE [MaterialApp], not inside its `home`.
  ///
  /// A pushed route and a dialog are mounted under the app's Navigator,
  /// which is above `home` — so a scope provided inside `home` is invisible
  /// to them. The onboarding wizard and the tray overlay are both routes,
  /// and both need the engine.
  @override
  Widget build(BuildContext context) => EngineScope(
        engine: widget.engine,
        child: BindingsScope(
          controller: _bindings,
          child: SettingsScope(
            settings: _settings,
            child: MaterialApp(
              title: 'Hand Controller',
              debugShowCheckedModeBanner: false,
              theme: buildHcTheme(),
              home: const AppShell(),
            ),
          ),
        ),
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
        body: Column(
          children: [
            // The window has no system title bar; this is it. See
            // `widgets/title_bar.dart`.
            const TitleBar(sidebarWidth: _Sidebar.width),
            Expanded(
              child: Row(
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
            ),
          ],
        ),
      );
}

class _Sidebar extends StatelessWidget {
  const _Sidebar({required this.current, required this.onSelect});

  final AppSection current;
  final ValueChanged<AppSection> onSelect;

  /// Fixed, and shared with the title bar so the two line up exactly.
  static const width = 208.0;

  @override
  Widget build(BuildContext context) => Container(
        width: width,
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
