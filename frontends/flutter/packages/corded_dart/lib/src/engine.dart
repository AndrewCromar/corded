import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:isolate';

import 'package:ffi/ffi.dart';

import 'bindings.dart';

/// A command the core refused, with its reason.
class CordedError implements Exception {
  CordedError(this.code, this.message);
  final String code;
  final String message;
  @override
  String toString() => message;
}

/// One running core: a vault, and connections to the servers it belongs to.
///
/// Commands are JSON objects, exactly as documented in corded.h. Everything the
/// core has to say arrives on [events], also as JSON objects.
class CordedEngine {
  CordedEngine._(this._bindings, this._engine, this._libraryPath);

  final Bindings _bindings;
  final Pointer<Void> _engine;
  final String? _libraryPath;
  final _events = StreamController<Map<String, dynamic>>.broadcast();
  final _pending = <int, Completer<Map<String, dynamic>>>{};
  final Pointer<Int32> _stop = calloc<Int32>();
  Completer<void>? _stopped;
  ReceivePort? _port;
  bool _closed = false;

  /// Starts a core whose vault lives in [vaultDir] (created on first use).
  ///
  /// A process has one core per vault: opening a vault that is already open
  /// in this process hands back the running core, still connected. With
  /// [pump] false nothing reads the core's events here; something else must
  /// (see [drain]) and pass them to [deliver].
  static Future<CordedEngine> open(String vaultDir,
      {bool fastKdf = false, String? libraryPath, bool pump = true}) async {
    final bindings = Bindings(openCordedLibrary(libraryPath));
    final config = calloc<CordedConfig>();
    final dir = vaultDir.toNativeUtf8();
    final out = calloc<Pointer<Void>>();
    try {
      config.ref
        ..structSize = sizeOf<CordedConfig>()
        ..vaultDir = dir
        ..fastKdf = fastKdf ? 1 : 0;
      final status = bindings.engineCreate(config, out);
      if (status != cordedOk) {
        throw CordedError('engine', bindings.statusMessage(status).toDartString());
      }
      final engine = CordedEngine._(bindings, out.value, libraryPath);
      if (pump) await engine.startPump();
      return engine;
    } finally {
      calloc.free(config);
      calloc.free(dir);
      calloc.free(out);
    }
  }

  /// Every event from the core, in order.
  Stream<Map<String, dynamic>> get events => _events.stream;

  String get version => _bindings.versionString().toDartString();
  int get apiVersion => _bindings.abiVersion() & 0xffff;

  bool vaultExists() {
    final out = calloc<Int32>();
    try {
      _check(_bindings.vaultExists(_engine, out));
      return out.value != 0;
    } finally {
      calloc.free(out);
    }
  }

  /// Makes a new identity on this device.
  Future<Map<String, dynamic>> createVault(String passphrase, String username) =>
      _withPassphrase(passphrase, (bytes, len, request) {
        final name = username.toNativeUtf8();
        try {
          return _bindings.vaultCreate(_engine, bytes, len, name, request);
        } finally {
          calloc.free(name);
        }
      });

  /// Sets this device up as a person who already exists on another device.
  Future<Map<String, dynamic>> restoreVault(String passphrase, String username, String recoveryKey) =>
      _withPassphrase(passphrase, (bytes, len, request) {
        final name = username.toNativeUtf8();
        final key = recoveryKey.toNativeUtf8();
        try {
          return _bindings.vaultRestore(_engine, bytes, len, name, key, request);
        } finally {
          calloc.free(name);
          calloc.free(key);
        }
      });

  Future<Map<String, dynamic>> unlock(String passphrase) => _withPassphrase(
      passphrase, (bytes, len, request) => _bindings.vaultUnlock(_engine, bytes, len, request));

  /// Sends a command and completes with the "data" of its result, or throws
  /// [CordedError] if the core or the server refused it.
  Future<Map<String, dynamic>> command(Map<String, dynamic> cmd) {
    final text = jsonEncode(cmd).toNativeUtf8();
    final request = calloc<Uint64>();
    try {
      _check(_bindings.command(_engine, text, text.length, request));
      return _await(request.value);
    } finally {
      calloc.free(text);
      calloc.free(request);
    }
  }

  /// Where the core lives in memory, for handing it to another isolate.
  int get address => _engine.address;

  /// Whether this object is reading the core's events itself.
  bool get pumping => _stopped != null;

  /// Stops reading events here, so that something else can (see [drain]).
  Future<void> stopPump() async {
    final stopped = _stopped;
    if (stopped == null) return;
    _stop.value = 1;
    await stopped.future;
    _port?.close();
    _port = null;
    _stopped = null;
    _stop.value = 0;
  }

  /// Takes every event the core has waiting, without blocking. For an isolate
  /// that reads events on behalf of a screen that may not exist: it calls this
  /// on a timer and passes each event on to [deliver] where there is one.
  static List<String> drain(int address, {String? libraryPath, int max = 500}) {
    final bindings = Bindings(openCordedLibrary(libraryPath));
    final engine = Pointer<Void>.fromAddress(address);
    final out = calloc<Pointer<Utf8>>();
    final events = <String>[];
    try {
      while (events.length < max && bindings.nextEvent(engine, 0, out, nullptr) == cordedOk) {
        events.add(out.value.toDartString());
        bindings.eventFree(out.value);
      }
    } finally {
      calloc.free(out);
    }
    return events;
  }

  /// Handles one event that was read elsewhere, as if it had been read here.
  void deliver(String json) => _handle(json);

  /// Stops the core. No events arrive after this completes.
  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    await stopPump();
    _bindings.engineDestroy(_engine);
    calloc.free(_stop);
    for (final c in _pending.values) {
      if (!c.isCompleted) c.completeError(CordedError('closed', 'the core was closed'));
    }
    _pending.clear();
    await _events.close();
  }

  // ---- internals

  void _check(int status) {
    if (status != cordedOk) throw CordedError('call', _bindings.statusMessage(status).toDartString());
  }

  Future<Map<String, dynamic>> _await(int request) {
    final completer = Completer<Map<String, dynamic>>();
    _pending[request] = completer;
    return completer.future;
  }

  Future<Map<String, dynamic>> _withPassphrase(
      String passphrase, int Function(Pointer<Uint8>, int, Pointer<Uint64>) call) {
    final bytes = utf8.encode(passphrase);
    final buffer = calloc<Uint8>(bytes.length + 1);
    final request = calloc<Uint64>();
    try {
      buffer.asTypedList(bytes.length).setAll(0, bytes);
      _check(call(buffer, bytes.length, request));
      return _await(request.value);
    } finally {
      buffer.asTypedList(bytes.length).fillRange(0, bytes.length, 0);
      calloc.free(buffer);
      calloc.free(request);
    }
  }

  // Waiting for an event blocks, so it happens on a thread of its own. The C
  // interface may be called from any thread.
  Future<void> startPump() async {
    if (_stopped != null || _closed) return;
    final stopped = _stopped = Completer<void>();
    final port = ReceivePort();
    _port = port;
    port.listen((message) {
      if (message == null) {
        if (!stopped.isCompleted) stopped.complete();
        return;
      }
      _handle(message as String);
    });
    await Isolate.spawn(_pump, [port.sendPort, _engine.address, _stop.address, _libraryPath]);
  }

  void _handle(String message) {
    {
      final Map<String, dynamic> event;
      try {
        event = jsonDecode(message) as Map<String, dynamic>;
      } catch (_) {
        return;
      }
      if (event['event'] == 'command_result') {
        final completer = _pending.remove(event['request']);
        if (completer != null) {
          if (event['ok'] == true) {
            completer.complete((event['data'] as Map?)?.cast<String, dynamic>() ?? <String, dynamic>{});
          } else {
            final error = (event['error'] as Map?) ?? const {};
            completer
                .completeError(CordedError('${error['code'] ?? 'error'}', '${error['message'] ?? 'failed'}'));
          }
        }
      }
      if (!_events.isClosed) _events.add(event);
    }
  }

  static void _pump(List<Object?> args) {
    final send = args[0] as SendPort;
    final engine = Pointer<Void>.fromAddress(args[1] as int);
    final stop = Pointer<Int32>.fromAddress(args[2] as int);
    final bindings = Bindings(openCordedLibrary(args[3] as String?));
    final out = calloc<Pointer<Utf8>>();
    while (stop.value == 0) {
      if (bindings.nextEvent(engine, 100, out, nullptr) != cordedOk) continue;
      send.send(out.value.toDartString());
      bindings.eventFree(out.value);
    }
    calloc.free(out);
    send.send(null);
  }
}
