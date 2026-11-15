/// Finding and opening `libhand_controller.so`, and the two string helpers
/// the C API needs.
///
/// Hand-rolled rather than `package:ffi` — `toNativeUtf8` is the only thing
/// that package would be used for here, and this project does not add a
/// dependency for twenty lines.
library;

import 'dart:convert';
import 'dart:ffi';
import 'dart:io';

import 'bindings.dart';

/// Thrown when the library is missing, unopenable, or the wrong version.
class HcLibraryUnavailable implements Exception {
  HcLibraryUnavailable(this.message);

  final String message;

  @override
  String toString() => message;
}

const _soname = 'libhand_controller.so';

/// Opens the library and checks that it is the one this code was written for.
///
/// The ABI check is not ceremony: a stale `.so` with a reordered struct reads
/// the wrong bytes and shows plausible-looking nonsense, with no error
/// anywhere. Better to refuse to start.
HcLibrary openHcLibrary({String? overridePath}) {
  final path = overridePath ?? _locate();
  if (path == null) {
    throw HcLibraryUnavailable(
      'Could not find $_soname. Looked beside the executable and in the '
      'build tree. Set HC_LIBRARY to its full path to override.',
    );
  }

  final DynamicLibrary dylib;
  try {
    dylib = DynamicLibrary.open(path);
  } on ArgumentError catch (e) {
    throw HcLibraryUnavailable('Could not open $path: $e');
  }

  final library = HcLibrary(dylib);
  final actual = library.abiVersion();
  if (actual != kHcAbiVersion) {
    throw HcLibraryUnavailable(
      'ABI mismatch: $path reports version $actual, this build expects '
      '$kHcAbiVersion. The library and the Dart bindings are out of step.',
    );
  }

  return library;
}

/// Where the library might be, nearest first.
///
/// Searched rather than hardcoded, and anchored to the EXECUTABLE as well as
/// the working directory: `flutter test` runs from `gui/`, but the built app
/// runs from six levels down inside `build/`, and a path relative to the cwd
/// is right in one case and wrong in the other.
String? _locate() {
  final fromEnv = Platform.environment['HC_LIBRARY'];
  if (fromEnv != null && fromEnv.isNotEmpty) return fromEnv;

  final exeDir = File(Platform.resolvedExecutable).parent;

  // Shipped: beside the binary, which is where a bundle puts it.
  for (final candidate in ['${exeDir.path}/lib/$_soname',
                           '${exeDir.path}/$_soname']) {
    if (File(candidate).existsSync()) return candidate;
  }

  // Development: walk up from both anchors looking for the engine's build
  // tree, either under this checkout or in the sibling worktree the C++ side
  // is built in.
  for (final start in [exeDir, Directory.current]) {
    var dir = start;
    for (var i = 0; i < 10; i++) {
      for (final candidate in [
        '${dir.path}/cpp/build-app/ffi/$_soname',
        '${dir.path}/Hand-Controller-Source/cpp/build-app/ffi/$_soname',
      ]) {
        if (File(candidate).existsSync()) return candidate;
      }
      if (dir.parent.path == dir.path) break;
      dir = dir.parent;
    }
  }

  return null;
}

/// Copies a Dart string into newly allocated NUL-terminated native memory.
///
/// The caller frees it. Every use below is scoped to a single call, because
/// the C side documents its string arguments as borrowed and copied
/// immediately.
Pointer<Uint8> toNativeUtf8(String value, Allocator allocator) {
  final bytes = utf8.encode(value);
  final pointer = allocator<Uint8>(bytes.length + 1);
  final view = pointer.asTypedList(bytes.length + 1);
  view.setRange(0, bytes.length, bytes);
  view[bytes.length] = 0;
  return pointer;
}

/// Reads a NUL-terminated string, stopping at [maxLength] bytes.
String fromNativeUtf8(Pointer<Uint8> pointer, int maxLength) {
  if (pointer == nullptr || maxLength <= 0) return '';
  final bytes = pointer.asTypedList(maxLength);
  var end = bytes.indexOf(0);
  if (end < 0) end = maxLength;
  return utf8.decode(bytes.sublist(0, end), allowMalformed: true);
}

/// Reads a fixed-size `char[]` field out of a struct.
String fromCharArray(Array<Uint8> array, int size) {
  final bytes = <int>[];
  for (var i = 0; i < size; i++) {
    final byte = array[i];
    if (byte == 0) break;
    bytes.add(byte);
  }
  return utf8.decode(bytes, allowMalformed: true);
}
