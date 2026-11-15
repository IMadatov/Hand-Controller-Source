import 'package:flutter/material.dart';

import '../app.dart';
import '../domain/bindings.dart';
import '../domain/command.dart';
import '../domain/gesture.dart';
import '../state/bindings_controller.dart';
import '../theme/tokens.dart';
import '../widgets/common.dart';

class BindingsScreen extends StatelessWidget {
  const BindingsScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final controller = BindingsScope.of(context);

    return AnimatedBuilder(
      animation: controller,
      builder: (context, _) {
        final draft = controller.draft;

        return Padding(
          padding: const EdgeInsets.all(HcSpace.xxl),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              ScreenHeader(
                title: 'Commands & Bindings',
                subtitle: 'What each pose does. Changes apply on save.',
                trailing: _SaveControls(controller: controller),
              ),
              if (draft == null)
                const Expanded(
                  child: Center(child: CircularProgressIndicator()),
                )
              else
                Expanded(
                  child: ListView(
                    children: [
                      if (controller.problems.isNotEmpty)
                        _ProblemBanner(problems: controller.problems),
                      if (controller.error != null)
                        _ErrorBanner(message: controller.error!),
                      _Group(
                        title: 'Right hand',
                        caption: 'Pointer control',
                        color: HcColors.handRight,
                        hand: HandSelector.right,
                        set: draft,
                      ),
                      const SizedBox(height: HcSpace.xl),
                      _Group(
                        title: 'Left hand',
                        caption: 'Keyboard control',
                        color: HcColors.handLeft,
                        hand: HandSelector.left,
                        set: draft,
                      ),
                      const SizedBox(height: HcSpace.xl),
                      _Group(
                        title: 'Either hand',
                        caption:
                            'Also matches a hand the model could not label',
                        color: HcColors.textMuted,
                        hand: HandSelector.any,
                        set: draft,
                      ),
                    ],
                  ),
                ),
            ],
          ),
        );
      },
    );
  }
}

class _SaveControls extends StatelessWidget {
  const _SaveControls({required this.controller});

  final BindingsController controller;

  @override
  Widget build(BuildContext context) {
    final dirty = controller.isDirty;
    final valid = controller.draft?.isValid ?? false;

    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        if (dirty)
          const Padding(
            padding: EdgeInsets.only(right: HcSpace.md),
            child: HcPill(label: 'unsaved', color: HcColors.warn),
          ),
        HcButton(
          label: 'Revert',
          icon: Icons.undo,
          onPressed: dirty ? controller.revert : null,
        ),
        const SizedBox(width: HcSpace.sm),
        HcButton(
          label: 'Save',
          icon: Icons.check,
          primary: true,
          tooltip: valid ? null : 'Fix the problems listed above first',
          onPressed: dirty && valid ? () => _save(context) : null,
        ),
      ],
    );
  }

  Future<void> _save(BuildContext context) async {
    // Saving drops disabled rows, because the format has no way to keep
    // them. Said before it happens rather than discovered afterwards.
    final dropped = controller.disabledRows;
    if (dropped.isNotEmpty) {
      final go = await showDialog<bool>(
        context: context,
        builder: (context) => _ConfirmDrop(count: dropped.length),
      );
      if (go != true) return;
    }

    final ok = await controller.save();
    if (!ok || !context.mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('Bindings saved')),
    );
  }
}

class _ConfirmDrop extends StatelessWidget {
  const _ConfirmDrop({required this.count});

  final int count;

  @override
  Widget build(BuildContext context) => AlertDialog(
        backgroundColor: HcColors.surfaceRaised,
        title: const Text('Disabled bindings will be removed'),
        content: Text(
          'The configuration file has no way to store a binding that is '
          'switched off, so saving deletes '
          '${count == 1 ? 'the 1 disabled row' : 'all $count disabled rows'}. '
          'Re-enable them first if you want to keep them.',
          style: Theme.of(context).textTheme.bodyMedium,
        ),
        actions: [
          HcButton(
            label: 'Cancel',
            onPressed: () => Navigator.of(context).pop(false),
          ),
          HcButton(
            label: 'Save and remove',
            primary: true,
            onPressed: () => Navigator.of(context).pop(true),
          ),
        ],
      );
}

class _ProblemBanner extends StatelessWidget {
  const _ProblemBanner({required this.problems});

  final List<BindingProblem> problems;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.only(bottom: HcSpace.lg),
        child: HcPanel(
          color: HcColors.dangerBackground,
          borderColor: HcColors.dangerBorder,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  const Icon(Icons.error_outline,
                      size: 15, color: HcColors.danger),
                  const SizedBox(width: HcSpace.md),
                  Text(
                    problems.length == 1
                        ? '1 problem to fix'
                        : '${problems.length} problems to fix',
                    style: const TextStyle(
                      fontFamily: HcFont.sans,
                      fontSize: 13,
                      fontWeight: FontWeight.w600,
                      color: HcColors.danger,
                    ),
                  ),
                ],
              ),
              const SizedBox(height: HcSpace.md),
              for (final problem in problems)
                Padding(
                  padding: const EdgeInsets.only(left: 25, bottom: 3),
                  child: Text(problem.message,
                      style: Theme.of(context).textTheme.bodySmall),
                ),
            ],
          ),
        ),
      );
}

class _ErrorBanner extends StatelessWidget {
  const _ErrorBanner({required this.message});

  final String message;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.only(bottom: HcSpace.lg),
        child: HcPanel(
          color: HcColors.dangerBackground,
          borderColor: HcColors.dangerBorder,
          child: Row(
            children: [
              const Icon(Icons.report_outlined,
                  size: 15, color: HcColors.danger),
              const SizedBox(width: HcSpace.md),
              Expanded(
                child: Text('The engine refused this: $message',
                    style: Theme.of(context).textTheme.bodySmall),
              ),
            ],
          ),
        ),
      );
}

/// One hand's bindings.
class _Group extends StatelessWidget {
  const _Group({
    required this.title,
    required this.caption,
    required this.color,
    required this.hand,
    required this.set,
  });

  final String title;
  final String caption;
  final Color color;
  final HandSelector hand;
  final BindingSet set;

  @override
  Widget build(BuildContext context) {
    // Generated releases are not rows. They are shown as part of the hold
    // that created them, so a user cannot half-delete a pair.
    final rows = set.bindings
        .where((b) =>
            b.hand == hand && b.command.kind != CommandKind.keyRelease)
        .toList()
      ..sort((a, b) => a.gesture.index.compareTo(b.gesture.index));

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Container(width: 3, height: 15, color: color),
            const SizedBox(width: HcSpace.md),
            Text(title, style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(width: HcSpace.md),
            Text(caption, style: Theme.of(context).textTheme.bodySmall),
            const Spacer(),
            HcButton(
              label: 'Add',
              icon: Icons.add,
              onPressed: () => _addRow(context),
            ),
          ],
        ),
        const SizedBox(height: HcSpace.md),
        if (rows.isEmpty)
          const HcPanel(
            child: Text(
              'Nothing bound to this hand',
              style: TextStyle(
                fontFamily: HcFont.sans,
                fontSize: 12,
                color: HcColors.textFaint,
              ),
            ),
          )
        else
          HcPanel(
            padding: EdgeInsets.zero,
            child: Column(
              children: [
                for (final (i, row) in rows.indexed) ...[
                  if (i > 0) const Divider(height: 1),
                  _BindingRow(binding: row, color: color),
                ],
              ],
            ),
          ),
      ],
    );
  }

  Future<void> _addRow(BuildContext context) async {
    final controller = BindingsScope.of(context);
    final used = set.bindings
        .where((b) => b.hand == hand)
        .map((b) => b.gesture)
        .toSet();
    final free =
        GestureKind.bindable.where((g) => !used.contains(g)).toList();

    if (free.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Every gesture is already bound here')),
      );
      return;
    }

    final gesture = await showDialog<GestureKind>(
      context: context,
      builder: (context) => _PickGesture(options: free),
    );
    if (gesture == null || !context.mounted) return;

    final command = await showDialog<HcCommand>(
      context: context,
      builder: (context) => const _RebindDialog(),
    );
    if (command == null) return;

    controller.add(Binding(
      hand: hand,
      gesture: gesture,
      phase: phaseFor(command.kind),
      command: command,
    ));
  }
}

class _BindingRow extends StatelessWidget {
  const _BindingRow({required this.binding, required this.color});

  final Binding binding;
  final Color color;

  @override
  Widget build(BuildContext context) {
    final controller = BindingsScope.of(context);
    final off = !binding.enabled;

    return Opacity(
      opacity: off ? 0.45 : 1,
      child: Padding(
        padding: const EdgeInsets.symmetric(
            horizontal: HcSpace.lg, vertical: HcSpace.md),
        child: Row(
          children: [
            // The pose.
            Expanded(
              flex: 4,
              child: Text(
                binding.gesture.label,
                style: const TextStyle(
                  fontFamily: HcFont.sans,
                  fontSize: 13,
                  fontWeight: FontWeight.w500,
                  color: HcColors.text,
                ),
              ),
            ),

            // When it fires. Shown, not editable — the phase follows from
            // the command.
            Expanded(
              flex: 2,
              child: Text(
                binding.phase.label,
                style: Theme.of(context).textTheme.bodySmall,
              ),
            ),

            const Icon(Icons.arrow_forward, size: 13, color: HcColors.textFaint),
            const SizedBox(width: HcSpace.lg),

            // What it does.
            Expanded(
              flex: 4,
              child: Row(
                children: [
                  Flexible(
                    child: Text(
                      binding.command.label,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                        fontFamily: HcFont.sans,
                        fontSize: 13,
                        fontWeight: FontWeight.w600,
                        color: color,
                      ),
                    ),
                  ),

                  // A hold always carries its release. Saying so is what
                  // makes the pairing visible rather than magic.
                  if (binding.command.kind == CommandKind.keyPress)
                    const Padding(
                      padding: EdgeInsets.only(left: HcSpace.sm),
                      child: HcPill(
                        label: 'auto-release',
                        color: HcColors.textMuted,
                      ),
                    ),
                ],
              ),
            ),

            HcButton(
              label: 'Rebind',
              onPressed: () => _rebind(context),
            ),
            const SizedBox(width: HcSpace.sm),
            IconButton(
              icon: const Icon(Icons.delete_outline, size: 16),
              color: HcColors.textFaint,
              tooltip: 'Remove this binding',
              onPressed: () => controller.remove(binding),
            ),
            Switch(
              value: binding.enabled,
              onChanged: (v) => controller.setEnabled(binding, v),
            ),
          ],
        ),
      ),
    );
  }

  Future<void> _rebind(BuildContext context) async {
    final controller = BindingsScope.of(context);
    final command = await showDialog<HcCommand>(
      context: context,
      builder: (context) => _RebindDialog(initial: binding.command),
    );
    if (command != null) controller.rebind(binding, command);
  }
}

class _PickGesture extends StatelessWidget {
  const _PickGesture({required this.options});

  final List<GestureKind> options;

  @override
  Widget build(BuildContext context) => SimpleDialog(
        backgroundColor: HcColors.surfaceRaised,
        title: const Text('Which pose?'),
        children: [
          for (final gesture in options)
            SimpleDialogOption(
              onPressed: () => Navigator.of(context).pop(gesture),
              child: Text(gesture.label,
                  style: Theme.of(context).textTheme.bodyMedium),
            ),
        ],
      );
}

/// Picks a command, and the one argument it needs.
///
/// There is no phase control here on purpose: see [phaseFor]. There is also
/// no `key_release` option — a release is generated with its hold, never
/// authored, which is what makes a stuck key unrepresentable rather than
/// merely discouraged.
class _RebindDialog extends StatefulWidget {
  const _RebindDialog({this.initial});

  final HcCommand? initial;

  @override
  State<_RebindDialog> createState() => _RebindDialogState();
}

class _RebindDialogState extends State<_RebindDialog> {
  late CommandKind _kind =
      widget.initial?.kind ?? CommandKind.moveCursor;
  late MouseButton _button = widget.initial?.button ?? MouseButton.left;
  late HcKey _key = widget.initial?.key ?? HcKey.arrowUp;

  @override
  Widget build(BuildContext context) => AlertDialog(
        backgroundColor: HcColors.surfaceRaised,
        title: const Text('What should this pose do?'),
        content: SizedBox(
          width: 380,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const HcLabel('COMMAND'),
              const SizedBox(height: HcSpace.sm),
              Wrap(
                spacing: HcSpace.sm,
                runSpacing: HcSpace.sm,
                children: [
                  for (final kind in CommandKind.authorable)
                    _Choice(
                      label: kind.label,
                      selected: kind == _kind,
                      onTap: () => setState(() => _kind = kind),
                    ),
                ],
              ),
              if (_kind.takesButton) ...[
                const SizedBox(height: HcSpace.lg),
                const HcLabel('BUTTON'),
                const SizedBox(height: HcSpace.sm),
                Wrap(
                  spacing: HcSpace.sm,
                  children: [
                    for (final button in MouseButton.values)
                      _Choice(
                        label: button.label,
                        selected: button == _button,
                        onTap: () => setState(() => _button = button),
                      ),
                  ],
                ),
              ],
              if (_kind.needsKey) ...[
                const SizedBox(height: HcSpace.lg),
                const HcLabel('KEY'),
                const SizedBox(height: HcSpace.sm),
                Wrap(
                  spacing: HcSpace.sm,
                  runSpacing: HcSpace.sm,
                  children: [
                    for (final key in HcKey.values)
                      _Choice(
                        label: key.label,
                        selected: key == _key,
                        onTap: () => setState(() => _key = key),
                      ),
                  ],
                ),
                const SizedBox(height: HcSpace.md),
                Row(
                  children: [
                    const Icon(Icons.info_outline,
                        size: 13, color: HcColors.textFaint),
                    const SizedBox(width: HcSpace.sm),
                    Expanded(
                      child: Text(
                        'The key is held while the pose is, and released '
                        'automatically when it stops.',
                        style: Theme.of(context).textTheme.bodySmall,
                      ),
                    ),
                  ],
                ),
              ],
              const SizedBox(height: HcSpace.lg),
              Text(
                'Fires ${phaseFor(_kind).label}.',
                style: Theme.of(context).textTheme.bodySmall,
              ),
            ],
          ),
        ),
        actions: [
          HcButton(
            label: 'Cancel',
            onPressed: () => Navigator.of(context).pop(),
          ),
          HcButton(
            label: 'Apply',
            primary: true,
            onPressed: () => Navigator.of(context).pop(
              HcCommand(
                kind: _kind,
                button: _button,
                key: _kind.needsKey ? _key : null,
              ),
            ),
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
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
            child: Text(
              label,
              style: TextStyle(
                fontFamily: HcFont.sans,
                fontSize: 12,
                fontWeight: FontWeight.w600,
                color: selected ? HcColors.textOnAccent : HcColors.textMuted,
              ),
            ),
          ),
        ),
      );
}
