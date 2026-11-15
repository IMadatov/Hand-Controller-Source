import 'package:flutter/foundation.dart';

import '../domain/bindings.dart';
import '../domain/command.dart';
import '../domain/gesture.dart';
import '../engine/engine.dart';

/// The bindings being edited.
///
/// Held above both the Commands screen and Settings, because the two edit
/// the same [BindingSet] — the sliders in Settings are `sensitivity` and
/// `smoothing`, which live in the configuration, not beside it.
///
/// It owns no file format. Reading and writing go through the engine, which
/// calls the C++ parser and serializer.
class BindingsController extends ChangeNotifier {
  BindingsController(this._engine);

  final HandControllerEngine _engine;

  BindingSet? _saved;
  BindingSet? _draft;
  bool _loading = true;
  String? _error;

  bool get isLoading => _loading;
  String? get error => _error;

  /// The set as edited. Null only before the first load completes.
  BindingSet? get draft => _draft;

  /// True when the draft differs from what the engine holds.
  bool get isDirty =>
      _draft != null && _saved != null && !_sameAs(_draft!, _saved!);

  List<BindingProblem> get problems => _draft?.problems ?? const [];

  /// Rows the user would lose by saving.
  ///
  /// The file format cannot say "present but disabled", so a disabled row is
  /// simply not written. That is a real deletion and the user is told before
  /// it happens, not after.
  List<Binding> get disabledRows =>
      _draft?.bindings.where((b) => !b.enabled).toList() ?? const [];

  Future<void> load() async {
    _loading = true;
    _error = null;
    notifyListeners();
    try {
      final set = await _engine.loadBindings();
      _saved = set;
      _draft = set;
    } on Object catch (e) {
      _error = '$e';
    }
    _loading = false;
    notifyListeners();
  }

  Future<bool> save() async {
    final draft = _draft;
    if (draft == null) return false;

    // Disabled rows are dropped on the way out, which is what makes them
    // disabled as far as the engine is concerned.
    final toWrite = draft.copyWith(
      bindings: draft.bindings.where((b) => b.enabled).toList(),
    );

    try {
      await _engine.saveBindings(toWrite);
      _saved = draft;
      _error = null;
      notifyListeners();
      return true;
    } on BindingsRejected catch (e) {
      _error = e.problems.join('\n');
      notifyListeners();
      return false;
    }
  }

  void revert() {
    _draft = _saved;
    _error = null;
    notifyListeners();
  }

  void _update(BindingSet next) {
    _draft = next;
    notifyListeners();
  }

  void setSensitivity(double value) {
    final d = _draft;
    if (d == null) return;
    _update(d.copyWith(sensitivity: value));
  }

  void setSmoothing(double value) {
    final d = _draft;
    if (d == null) return;
    _update(d.copyWith(smoothing: value));
  }

  /// Turns a row on or off, carrying its generated release with it.
  ///
  /// A press enabled without its release would be exactly the stuck-key
  /// configuration the whole design refuses to produce.
  void setEnabled(Binding binding, bool enabled) {
    final d = _draft;
    if (d == null) return;

    _update(d.copyWith(
      bindings: [
        for (final b in d.bindings)
          if (b == binding || _isPartnerOf(binding, b))
            b.copyWith(enabled: enabled)
          else
            b,
      ],
    ));
  }

  /// Replaces a row's command, rebuilding its release partner.
  void rebind(Binding binding, HcCommand command) {
    final d = _draft;
    if (d == null) return;

    final replacement = binding.copyWith(
      command: command,
      phase: phaseFor(command.kind),
    );

    final next = <Binding>[
      for (final b in d.bindings)
        if (b == binding)
          replacement
        else if (!_isPartnerOf(binding, b))
          b,
    ];

    if (command.kind == CommandKind.keyPress) {
      next.add(BindingSet.releaseFor(replacement));
    }

    _update(d.copyWith(bindings: next));
  }

  void add(Binding binding) {
    final d = _draft;
    if (d == null) return;

    // ONE COMMAND PER (hand, gesture) in this editor, not per (hand, gesture,
    // phase). The file format can hold a pose that scrolls while held AND
    // clicks when it starts, but the editor shows one row per pose, so a
    // second command on a different phase would appear as a duplicate row
    // the user cannot tell apart. A hold's generated release is the one
    // exception, and it is appended below rather than authored.
    final next = <Binding>[
      for (final b in d.bindings)
        if (!(b.hand == binding.hand && b.gesture == binding.gesture)) b,
      binding,
    ];
    if (binding.command.kind == CommandKind.keyPress) {
      next.add(BindingSet.releaseFor(binding));
    }

    _update(d.copyWith(bindings: next));
  }

  void remove(Binding binding) {
    final d = _draft;
    if (d == null) return;
    _update(d.copyWith(
      bindings: [
        for (final b in d.bindings)
          if (b != binding && !_isPartnerOf(binding, b)) b,
      ],
    ));
  }

  /// True when [candidate] is the `key_release` generated for [press].
  bool _isPartnerOf(Binding press, Binding candidate) =>
      press.command.kind == CommandKind.keyPress &&
      candidate.command.kind == CommandKind.keyRelease &&
      candidate.hand == press.hand &&
      candidate.gesture == press.gesture &&
      candidate.phase == GesturePhase.ended &&
      candidate.command.key == press.command.key;

  static bool _sameAs(BindingSet a, BindingSet b) =>
      a.sensitivity == b.sensitivity &&
      a.smoothing == b.smoothing &&
      a.bindings.length == b.bindings.length &&
      !a.bindings.indexed.any((e) => b.bindings[e.$1] != e.$2);
}

/// The phase a command must fire on.
///
/// DERIVED, NOT CHOSEN. The editor never asks the user for a phase, because
/// the right answer follows from the command every time: continuous commands
/// act while the pose is held, one-shot commands act when it starts, and a
/// release is generated rather than authored. Exposing the choice would add
/// a control whose only use is to produce a configuration that does not work
/// — a click that fires 28 times a second, or a key that is never released.
GesturePhase phaseFor(CommandKind kind) => switch (kind) {
      CommandKind.moveCursor || CommandKind.scroll => GesturePhase.held,
      CommandKind.keyRelease => GesturePhase.ended,
      _ => GesturePhase.began,
    };
