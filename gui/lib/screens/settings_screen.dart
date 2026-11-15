import 'package:flutter/material.dart';

import '../app.dart';
import '../domain/bindings.dart';
import '../domain/observation.dart';
import '../state/app_settings.dart';
import '../state/bindings_controller.dart';
import '../theme/tokens.dart';
import '../widgets/common.dart';

enum _Category {
  camera('Camera', Icons.videocam_outlined),
  input('Input & Cursor', Icons.mouse_outlined),
  detection('Detection', Icons.center_focus_strong_outlined),
  system('System', Icons.settings_outlined),
  language('Language', Icons.translate_outlined);

  const _Category(this.label, this.icon);

  final String label;
  final IconData icon;
}

class SettingsScreen extends StatefulWidget {
  const SettingsScreen({super.key});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  var _category = _Category.camera;
  /// Null while enumeration is in flight; empty means none exist.
  List<CameraDevice>? _cameras;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_cameras != null) return;
    EngineScope.of(context).enumerateCameras().then((c) {
      if (mounted) setState(() => _cameras = c);
    });
  }

  @override
  Widget build(BuildContext context) {
    final settings = SettingsScope.of(context);
    final bindings = BindingsScope.of(context);

    return AnimatedBuilder(
      animation: Listenable.merge([settings, bindings]),
      builder: (context, _) => Padding(
        padding: const EdgeInsets.all(HcSpace.xxl),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            ScreenHeader(
              title: 'Settings',
              subtitle: 'Not saved between runs yet',
              trailing: _SaveRow(bindings: bindings),
            ),
            Expanded(
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  SizedBox(
                    width: 180,
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        for (final category in _Category.values)
                          _CategoryTab(
                            category: category,
                            selected: category == _category,
                            onTap: () =>
                                setState(() => _category = category),
                          ),
                      ],
                    ),
                  ),
                  const SizedBox(width: HcSpace.xl),
                  Expanded(
                    child: ListView(
                      children: switch (_category) {
                        _Category.camera => _cameraSection(settings),
                        _Category.input => _inputSection(settings, bindings),
                        _Category.detection => _detectionSection(settings),
                        _Category.system => _systemSection(settings),
                        _Category.language => _languageSection(settings),
                      },
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

  List<Widget> _cameraSection(AppSettings settings) => [
        _Section(
          title: 'Camera',
          children: [
            _Field(
              label: 'Device',
              help: (_cameras?.isEmpty ?? false)
                  ? 'No /dev/video* device is present on this machine.'
                  : null,
              child: switch (_cameras) {
                null => const Text(
                    'Looking for cameras',
                    style: TextStyle(
                      fontFamily: HcFont.sans,
                      fontSize: 12,
                      color: HcColors.textFaint,
                    ),
                  ),
                [] => const Text(
                    'No camera found',
                    style: TextStyle(
                      fontFamily: HcFont.sans,
                      fontSize: 12,
                      color: HcColors.textFaint,
                    ),
                  ),
                final found => _Choices(
                    options: [for (final c in found) c.name],
                    selected: found.first.name,
                    onSelect: (_) {},
                  ),
              },
            ),
            _Field(
              label: 'Resolution',
              help: 'Lowering this is the first thing to try when the frame '
                  'rate is too low.',
              child: _Choices(
                options: [
                  for (final r in CaptureResolution.values) r.label,
                ],
                selected: settings.resolution.label,
                onSelect: (label) => settings.update(() => settings.resolution =
                    CaptureResolution.values
                        .firstWhere((r) => r.label == label)),
              ),
            ),
          ],
        ),
      ];

  List<Widget> _inputSection(
      AppSettings settings, BindingsController bindings) {
    final draft = bindings.draft;

    return [
      _Section(
        title: 'Cursor',
        // Said plainly because these two controls behave differently from
        // every other control on this screen: they are written to the
        // bindings file, so they need a save.
        note: 'Part of the bindings configuration. Save applies them.',
        children: [
          _Field(
            label: 'Cursor sensitivity',
            help: 'Multiplies hand movement. Above 1 so a small movement can '
                'cross a wide display. Chosen, not measured — tune it.',
            child: _Slider(
              value: draft?.sensitivity ?? kDefaultSensitivity,
              min: 0.2,
              max: 6,
              suffix: 'x',
              onChanged: bindings.setSensitivity,
            ),
          ),
          _Field(
            label: 'Smoothing',
            help: '0 is raw and jittery. Near 1 the cursor barely responds.',
            child: _Slider(
              value: draft?.smoothing ?? kDefaultSmoothing,
              min: 0,
              max: 1,
              onChanged: bindings.setSmoothing,
            ),
          ),
        ],
      ),
      _Section(
        title: 'Scroll',
        children: [
          _Field(
            label: 'Scroll sensitivity',
            help: 'Separate from the cursor: a comfortable pointer speed and '
                'a comfortable scroll speed are not the same number.',
            child: _Slider(
              value: settings.scrollSensitivity,
              min: 0.2,
              max: 4,
              suffix: 'x',
              onChanged: (v) =>
                  settings.update(() => settings.scrollSensitivity = v),
            ),
          ),
        ],
      ),
    ];
  }

  List<Widget> _detectionSection(AppSettings settings) => [
        _Section(
          title: 'Detection',
          children: [
            _Field(
              label: 'Threshold',
              help: settings.threshold.note,
              child: _Choices(
                options: [for (final t in DetectionThreshold.values) t.label],
                selected: settings.threshold.label,
                onSelect: (label) => settings.update(() => settings.threshold =
                    DetectionThreshold.values
                        .firstWhere((t) => t.label == label)),
              ),
            ),
            _Toggle(
              label: 'Show numeric values',
              help: 'Print the raw number beside each slider and score.',
              value: settings.showNumericValues,
              onChanged: (v) =>
                  settings.update(() => settings.showNumericValues = v),
            ),
          ],
        ),
      ];

  List<Widget> _systemSection(AppSettings settings) => [
        _Section(
          title: 'System',
          children: [
            _Toggle(
              label: 'Launch at startup',
              help: 'Start tracking when you log in.',
              value: settings.launchAtStartup,
              onChanged: (v) =>
                  settings.update(() => settings.launchAtStartup = v),
            ),
            _Field(
              label: 'Panic hotkey',
              // The reason this control is not optional.
              help: 'Stops everything from the keyboard alone. Gesture input '
                  'can fail in ways you cannot click your way out of — a key '
                  'stuck down, or the cursor being driven away from the stop '
                  'button.',
              child: Row(
                children: [
                  for (final part in settings.panicHotkey.split(' + '))
                    Padding(
                      padding: const EdgeInsets.only(right: HcSpace.sm),
                      child: _Key(part),
                    ),
                  const SizedBox(width: HcSpace.sm),
                  HcButton(label: 'Change', onPressed: () {}),
                ],
              ),
            ),
          ],
        ),
      ];

  List<Widget> _languageSection(AppSettings settings) => [
        _Section(
          title: 'Language',
          children: [
            _Field(
              label: 'Interface language',
              child: _Choices(
                options: [for (final l in UiLanguage.values) l.label],
                selected: settings.language.label,
                onSelect: (label) => settings.update(() => settings.language =
                    UiLanguage.values.firstWhere((l) => l.label == label)),
              ),
            ),
          ],
        ),
      ];
}

class _SaveRow extends StatelessWidget {
  const _SaveRow({required this.bindings});

  final BindingsController bindings;

  @override
  Widget build(BuildContext context) => Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          if (bindings.isDirty)
            const Padding(
              padding: EdgeInsets.only(right: HcSpace.md),
              child: HcPill(label: 'unsaved', color: HcColors.warn),
            ),
          HcButton(
            label: 'Revert',
            icon: Icons.undo,
            onPressed: bindings.isDirty ? bindings.revert : null,
          ),
          const SizedBox(width: HcSpace.sm),
          HcButton(
            label: 'Save',
            icon: Icons.check,
            primary: true,
            onPressed: bindings.isDirty ? bindings.save : null,
          ),
        ],
      );
}

class _CategoryTab extends StatelessWidget {
  const _CategoryTab({
    required this.category,
    required this.selected,
    required this.onTap,
  });

  final _Category category;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.only(bottom: 2),
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
                  Icon(category.icon,
                      size: 15,
                      color:
                          selected ? HcColors.accent : HcColors.textFaint),
                  const SizedBox(width: HcSpace.md),
                  Text(
                    category.label,
                    style: TextStyle(
                      fontFamily: HcFont.sans,
                      fontSize: 12,
                      fontWeight:
                          selected ? FontWeight.w600 : FontWeight.w500,
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

class _Section extends StatelessWidget {
  const _Section({required this.title, required this.children, this.note});

  final String title;
  final String? note;
  final List<Widget> children;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.only(bottom: HcSpace.xl),
        child: HcPanel(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(title, style: Theme.of(context).textTheme.titleMedium),
              if (note != null) ...[
                const SizedBox(height: 3),
                Text(note!, style: Theme.of(context).textTheme.bodySmall),
              ],
              const SizedBox(height: HcSpace.lg),
              ...children,
            ],
          ),
        ),
      );
}

class _Field extends StatelessWidget {
  const _Field({required this.label, required this.child, this.help});

  final String label;
  final String? help;
  final Widget child;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.only(bottom: HcSpace.xl),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(label, style: Theme.of(context).textTheme.titleSmall),
            if (help != null) ...[
              const SizedBox(height: 3),
              Text(help!, style: Theme.of(context).textTheme.bodySmall),
            ],
            const SizedBox(height: HcSpace.md),
            child,
          ],
        ),
      );
}

class _Toggle extends StatelessWidget {
  const _Toggle({
    required this.label,
    required this.value,
    required this.onChanged,
    this.help,
  });

  final String label;
  final String? help;
  final bool value;
  final ValueChanged<bool> onChanged;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.only(bottom: HcSpace.lg),
        child: Row(
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(label,
                      style: Theme.of(context).textTheme.titleSmall),
                  if (help != null) ...[
                    const SizedBox(height: 3),
                    Text(help!,
                        style: Theme.of(context).textTheme.bodySmall),
                  ],
                ],
              ),
            ),
            Switch(value: value, onChanged: onChanged),
          ],
        ),
      );
}

class _Choices extends StatelessWidget {
  const _Choices({
    required this.options,
    required this.selected,
    required this.onSelect,
  });

  final List<String> options;
  final String selected;
  final ValueChanged<String> onSelect;

  @override
  Widget build(BuildContext context) => Wrap(
        spacing: HcSpace.sm,
        runSpacing: HcSpace.sm,
        children: [
          for (final option in options)
            Material(
              color: option == selected
                  ? HcColors.accent
                  : HcColors.surfaceActive,
              borderRadius: BorderRadius.circular(HcRadius.md),
              child: InkWell(
                onTap: () => onSelect(option),
                borderRadius: BorderRadius.circular(HcRadius.md),
                child: Padding(
                  padding: const EdgeInsets.symmetric(
                      horizontal: 12, vertical: 7),
                  child: Text(
                    option,
                    style: TextStyle(
                      fontFamily: HcFont.sans,
                      fontSize: 12,
                      fontWeight: FontWeight.w600,
                      color: option == selected
                          ? HcColors.textOnAccent
                          : HcColors.textMuted,
                    ),
                  ),
                ),
              ),
            ),
        ],
      );
}

class _Slider extends StatelessWidget {
  const _Slider({
    required this.value,
    required this.min,
    required this.max,
    required this.onChanged,
    this.suffix = '',
  });

  final double value;
  final double min;
  final double max;
  final String suffix;
  final ValueChanged<double> onChanged;

  @override
  Widget build(BuildContext context) => Row(
        children: [
          Expanded(
            child: Slider(
              value: value.clamp(min, max),
              min: min,
              max: max,
              onChanged: onChanged,
            ),
          ),
          const SizedBox(width: HcSpace.md),
          SizedBox(
            width: 52,
            child: Text(
              '${value.toStringAsFixed(2)}$suffix',
              textAlign: TextAlign.right,
              style: const TextStyle(
                fontFamily: HcFont.mono,
                fontSize: 12,
                fontWeight: FontWeight.w600,
                color: HcColors.text,
                fontFeatures: [FontFeature.tabularFigures()],
              ),
            ),
          ),
        ],
      );
}

class _Key extends StatelessWidget {
  const _Key(this.label);

  final String label;

  @override
  Widget build(BuildContext context) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
        decoration: BoxDecoration(
          color: HcColors.surfaceActive,
          borderRadius: BorderRadius.circular(HcRadius.sm + 1),
          border: Border.all(color: HcColors.borderStrong),
        ),
        child: Text(
          label,
          style: const TextStyle(
            fontFamily: HcFont.sans,
            fontSize: 11,
            fontWeight: FontWeight.w600,
            color: HcColors.text,
          ),
        ),
      );
}
