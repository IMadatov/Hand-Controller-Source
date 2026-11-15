/// Reads and writes the bindings file format in Dart.
///
/// THIS FILE IS A GAP, NOT A DESIGN, and it is the one place in this app that
/// does what the contract says a UI must not do.
///
/// The C ABI exposes the bindings only as TEXT —
/// `hc_session_bindings_text` and `hc_session_set_bindings_text`. The editor
/// renders and edits ROWS, so something has to turn one into the other, and
/// with only those two calls available that something is here. The contract's
/// own reasoning (§5: "two definitions drift on the first edit to either")
/// applies to this file in full.
///
/// **What would remove it:** a structured pair on the C side —
/// `hc_session_binding_count` / `hc_session_binding_at(i, hc_binding*)` to
/// read, and `hc_session_set_bindings(const hc_binding*, int32_t count,
/// float sensitivity, float smoothing, char* error, int32_t cap)` to write.
/// Then the format never leaves C++ and this file is deleted outright. It is
/// raised with the engine session; until it lands, drift is contained by
/// [FfiEngine.saveBindings] writing through C++ and then RE-READING the
/// engine's own text, so the engine always has the last word and any
/// disagreement shows up immediately as the editor snapping back.
library;

import '../../domain/bindings.dart';
import '../../domain/command.dart';
import '../../domain/gesture.dart';

/// Thrown when the text cannot be read. The engine's parser is the real
/// authority; this one only has to be good enough to render.
class BindingsTextError implements Exception {
  BindingsTextError(this.message, this.line);

  final String message;
  final int line;

  @override
  String toString() => 'line $line: $message';
}

BindingSet parseBindingsText(String text) {
  final bindings = <Binding>[];
  var sensitivity = kDefaultSensitivity;
  var smoothing = kDefaultSmoothing;

  final lines = text.split('\n');
  for (var i = 0; i < lines.length; i++) {
    final lineNumber = i + 1;

    // Comments run to end of line, and a line that is only a comment is not
    // an error.
    final hash = lines[i].indexOf('#');
    final line = (hash < 0 ? lines[i] : lines[i].substring(0, hash)).trim();
    if (line.isEmpty) continue;

    final words = line.split(RegExp(r'\s+'));

    if (words.first == 'set') {
      if (words.length != 3) {
        throw BindingsTextError('a `set` needs a name and a value', lineNumber);
      }
      final value = double.tryParse(words[2]);
      if (value == null) {
        throw BindingsTextError('${words[2]} is not a number', lineNumber);
      }
      switch (words[1]) {
        case 'sensitivity':
          sensitivity = value;
        case 'smoothing':
          smoothing = value;
        default:
          throw BindingsTextError('unknown setting ${words[1]}', lineNumber);
      }
      continue;
    }

    final arrow = words.indexOf('->');
    if (arrow < 0) {
      throw BindingsTextError('expected `->`', lineNumber);
    }
    if (arrow != 3) {
      throw BindingsTextError(
          'expected <hand> <gesture> <phase> before `->`', lineNumber);
    }

    final hand = HandSelector.fromWire(words[0]);
    final gesture = GestureKind.fromWire(words[1]);
    final phase = GesturePhase.fromWire(words[2]);
    if (hand == null) {
      throw BindingsTextError('unknown hand ${words[0]}', lineNumber);
    }
    if (gesture == null) {
      throw BindingsTextError('unknown gesture ${words[1]}', lineNumber);
    }
    if (phase == null) {
      throw BindingsTextError('unknown phase ${words[2]}', lineNumber);
    }

    final rest = words.sublist(arrow + 1);
    if (rest.isEmpty) {
      throw BindingsTextError('no command after `->`', lineNumber);
    }

    final kind = CommandKind.fromWire(rest.first);
    if (kind == null) {
      throw BindingsTextError('unknown command ${rest.first}', lineNumber);
    }

    var button = MouseButton.left;
    HcKey? key;

    if (rest.length > 1) {
      final argument = rest[1];
      if (kind.needsKey) {
        key = HcKey.fromWire(argument);
        if (key == null) {
          throw BindingsTextError('unknown key $argument', lineNumber);
        }
      } else if (kind.takesButton) {
        final parsed = MouseButton.fromWire(argument);
        if (parsed == null) {
          throw BindingsTextError('unknown button $argument', lineNumber);
        }
        button = parsed;
      } else {
        throw BindingsTextError(
            '${kind.wire} takes no argument', lineNumber);
      }
    } else if (kind.needsKey) {
      throw BindingsTextError('${kind.wire} needs a key', lineNumber);
    }

    bindings.add(Binding(
      hand: hand,
      gesture: gesture,
      phase: phase,
      command: HcCommand(kind: kind, button: button, key: key),
    ));
  }

  return BindingSet(
    bindings: bindings,
    sensitivity: sensitivity,
    smoothing: smoothing,
  );
}

/// Writes [set] in the file format.
///
/// Disabled rows are omitted, which is what makes them disabled as far as
/// the engine is concerned — the format has no way to store one, and the
/// editor warns before a save that would drop any.
String serializeBindingsText(BindingSet set) {
  final out = StringBuffer()
    ..writeln('# Written by Hand Controller.')
    ..writeln()
    ..writeln('set sensitivity ${_number(set.sensitivity)}')
    ..writeln('set smoothing ${_number(set.smoothing)}')
    ..writeln();

  for (final binding in set.bindings) {
    if (!binding.enabled) continue;
    final argument = binding.command.wireArgument;
    out.writeln(
      '${binding.hand.wire} ${binding.gesture.wire} ${binding.phase.wire} '
      '-> ${binding.command.kind.wire}${argument == null ? '' : ' $argument'}',
    );
  }

  return out.toString();
}

/// Trims a trailing `.0` so a round trip does not churn the file.
String _number(double value) {
  final text = value.toStringAsFixed(3);
  final trimmed = text.replaceFirst(RegExp(r'0+$'), '');
  return trimmed.endsWith('.') ? '${trimmed}0' : trimmed;
}
