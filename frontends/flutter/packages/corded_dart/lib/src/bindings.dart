// The C interface in include/corded/corded.h, as Dart sees it.
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

final class CordedConfig extends Struct {
  @Uint32()
  external int structSize;
  external Pointer<Utf8> vaultDir;
  @Int32()
  external int fastKdf;
}

const int cordedOk = 0;

class Bindings {
  Bindings(DynamicLibrary lib)
      : abiVersion = lib.lookupFunction<Uint32 Function(), int Function()>('corded_abi_version'),
        versionString =
            lib.lookupFunction<Pointer<Utf8> Function(), Pointer<Utf8> Function()>('corded_version_string'),
        statusMessage = lib.lookupFunction<Pointer<Utf8> Function(Int32), Pointer<Utf8> Function(int)>(
            'corded_status_message'),
        engineCreate = lib.lookupFunction<Int32 Function(Pointer<CordedConfig>, Pointer<Pointer<Void>>),
            int Function(Pointer<CordedConfig>, Pointer<Pointer<Void>>)>('corded_engine_create'),
        engineDestroy = lib.lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>(
            'corded_engine_destroy'),
        vaultExists = lib.lookupFunction<Int32 Function(Pointer<Void>, Pointer<Int32>),
            int Function(Pointer<Void>, Pointer<Int32>)>('corded_vault_exists'),
        vaultCreate = lib.lookupFunction<
            Int32 Function(Pointer<Void>, Pointer<Uint8>, Size, Pointer<Utf8>, Pointer<Uint64>),
            int Function(
                Pointer<Void>, Pointer<Uint8>, int, Pointer<Utf8>, Pointer<Uint64>)>('corded_vault_create'),
        vaultRestore = lib.lookupFunction<
            Int32 Function(
                Pointer<Void>, Pointer<Uint8>, Size, Pointer<Utf8>, Pointer<Utf8>, Pointer<Uint64>),
            int Function(Pointer<Void>, Pointer<Uint8>, int, Pointer<Utf8>, Pointer<Utf8>,
                Pointer<Uint64>)>('corded_vault_restore'),
        vaultUnlock = lib.lookupFunction<Int32 Function(Pointer<Void>, Pointer<Uint8>, Size, Pointer<Uint64>),
            int Function(Pointer<Void>, Pointer<Uint8>, int, Pointer<Uint64>)>('corded_vault_unlock'),
        command = lib.lookupFunction<Int32 Function(Pointer<Void>, Pointer<Utf8>, Size, Pointer<Uint64>),
            int Function(Pointer<Void>, Pointer<Utf8>, int, Pointer<Uint64>)>('corded_command'),
        nextEvent = lib.lookupFunction<
            Int32 Function(Pointer<Void>, Int32, Pointer<Pointer<Utf8>>, Pointer<Size>),
            int Function(Pointer<Void>, int, Pointer<Pointer<Utf8>>, Pointer<Size>)>('corded_next_event'),
        eventFree = lib
            .lookupFunction<Void Function(Pointer<Utf8>), void Function(Pointer<Utf8>)>('corded_event_free');

  final int Function() abiVersion;
  final Pointer<Utf8> Function() versionString;
  final Pointer<Utf8> Function(int) statusMessage;
  final int Function(Pointer<CordedConfig>, Pointer<Pointer<Void>>) engineCreate;
  final void Function(Pointer<Void>) engineDestroy;
  final int Function(Pointer<Void>, Pointer<Int32>) vaultExists;
  final int Function(Pointer<Void>, Pointer<Uint8>, int, Pointer<Utf8>, Pointer<Uint64>) vaultCreate;
  final int Function(Pointer<Void>, Pointer<Uint8>, int, Pointer<Utf8>, Pointer<Utf8>, Pointer<Uint64>)
      vaultRestore;
  final int Function(Pointer<Void>, Pointer<Uint8>, int, Pointer<Uint64>) vaultUnlock;
  final int Function(Pointer<Void>, Pointer<Utf8>, int, Pointer<Uint64>) command;
  final int Function(Pointer<Void>, int, Pointer<Pointer<Utf8>>, Pointer<Size>) nextEvent;
  final void Function(Pointer<Utf8>) eventFree;
}

/// Opens libcorded. On a phone it ships inside the app; on a desktop it sits
/// next to the program unless [path] says otherwise.
DynamicLibrary openCordedLibrary([String? path]) {
  if (path != null) return DynamicLibrary.open(path);
  if (Platform.isWindows) return DynamicLibrary.open('corded.dll');
  if (Platform.isMacOS || Platform.isIOS) return DynamicLibrary.process();
  if (Platform.isLinux) {
    // A desktop app carries the core in the "lib" folder beside its program.
    final bundled = '${File(Platform.resolvedExecutable).parent.path}/lib/libcorded.so';
    if (File(bundled).existsSync()) return DynamicLibrary.open(bundled);
  }
  return DynamicLibrary.open('libcorded.so');
}
