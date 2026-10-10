import 'dart:async';
import 'dart:io';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/foundation.dart';
import 'package:characters/characters.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'background.dart';
import 'fingerprint.dart';

/// What the whole app shares: the running core and the lists built from it.
/// The app keeps nothing of its own; the core's vault is the only store.
class AppState extends ChangeNotifier {
  CordedEngine? _engine;
  CordedStore? _store;
  StreamSubscription<void>? _sub;
  String? startError;
  int? _selectedServer;
  bool _wantBackground = false;

  /// The quick reactions offered on a message; the first is what a double-tap
  /// sends. How the app looks is the one thing kept outside the vault.
  static const defaultReactions = ['👍', '❤️', '😂', '😮', '😢', '🎉'];
  List<String> reactionBar = defaultReactions;

  /// Reads emoji out of free text: one per visible character, no blanks or repeats.
  static List<String> parseReactions(String text) {
    final out = <String>[];
    for (final c in text.characters) {
      if (c.trim().isEmpty || out.contains(c)) continue;
      out.add(c);
    }
    return out.take(8).toList();
  }

  Future<void> setReactionBar(List<String> bar) async {
    reactionBar = bar.isEmpty ? defaultReactions : bar;
    notifyListeners();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setStringList('reaction_bar', reactionBar);
  }

  CordedEngine get engine => _engine!;
  CordedStore get store => _store!;
  bool get ready => _store != null;

  /// Whether the vault can be opened with a fingerprint instead of typing.
  bool fingerprintUnlock = false;
  bool fingerprintAvailable = false;

  /// Turns fingerprint unlock on, after checking the passphrase is right.
  /// Throws [CordedError] if it is not.
  Future<void> enableFingerprint(String passphrase) async {
    await engine.unlock(passphrase); // the vault is open, so this only checks it
    await Fingerprint.save(passphrase);
    fingerprintUnlock = true;
    (await SharedPreferences.getInstance()).setBool('fingerprint_unlock', true);
    notifyListeners();
  }

  Future<void> disableFingerprint() async {
    await Fingerprint.clear();
    fingerprintUnlock = false;
    (await SharedPreferences.getInstance()).setBool('fingerprint_unlock', false);
    notifyListeners();
  }

  /// Opens the vault with a fingerprint. Returns a sentence to show if it
  /// did not work, or null if it did or the person backed out.
  Future<String?> unlockWithFingerprint() async {
    final String? passphrase;
    try {
      passphrase = await Fingerprint.read();
    } catch (e) {
      return 'The fingerprint check did not work. Type your passphrase instead.';
    }
    if (passphrase == null) return null;
    try {
      await engine.unlock(passphrase);
      return null;
    } on CordedError {
      // The stored passphrase no longer opens the vault; stop offering it.
      await disableFingerprint();
      return 'Fingerprint unlock was turned off because the stored passphrase no longer works.';
    }
  }

  /// Whether the app keeps its connections while it is not on screen.
  bool backgroundMode = false;

  /// Turns the background connection on or off. Returns a sentence saying
  /// why it could not be turned on, or null.
  Future<String?> setBackgroundMode(bool on) async {
    final prefs = await SharedPreferences.getInstance();
    if (on) {
      // From here the service's task reads the core's events, not this screen.
      await engine.stopPump();
      final problem = await Background.start(engine.address);
      if (problem != null) {
        await engine.startPump();
        return problem;
      }
      await Background.askToRunUnrestricted();
    } else {
      await Background.stop();
      await engine.startPump();
    }
    backgroundMode = on;
    await prefs.setBool('background_mode', on);
    if (on) {
      // The task takes a moment to start listening.
      Future<void>.delayed(const Duration(seconds: 2), () => _shareRoomTitles(force: true));
    }
    notifyListeners();
    return null;
  }

  // The background task titles its notifications with the names of the chats.
  String _sharedTitles = '';
  void _shareRoomTitles({bool force = false}) {
    if (!backgroundMode) return;
    final titles = {for (final r in store.rooms.values) r.id: r.title};
    final key = titles.toString();
    if (!force && key == _sharedTitles) return;
    _sharedTitles = key;
    Background.tell({'rooms': titles});
  }

  /// Where a tapped notification wants to go, until the app can go there
  /// (the vault may still be locked, or the chats not listed yet).
  Map<String, dynamic>? _pendingOpen;
  void Function(String roomId, String? threadRoot)? onOpenChat;

  void openFromNotification(Map<String, dynamic> target) {
    _pendingOpen = target;
    _tryPendingOpen();
  }

  void _tryPendingOpen() {
    final target = _pendingOpen;
    if (target == null || !ready || store.vaultState != 'unlocked' || onOpenChat == null) return;
    final room = store.rooms[target['room_id']];
    if (room == null) return;
    _pendingOpen = null;
    _selectedServer = room.serverId;
    notifyListeners();
    onOpenChat!(room.id, target['thread'] as String?);
  }

  /// Tells the background task whether someone is looking at the app.
  void setOnScreen(bool onScreen) => Background.tell(onScreen ? 'on_screen' : 'off_screen');

  Future<void> start() async {
    Background.init();
    try {
      final saved = (await SharedPreferences.getInstance()).getStringList('reaction_bar');
      if (saved != null && saved.isNotEmpty) reactionBar = saved;
    } catch (_) {
      // The defaults will do.
    }
    try {
      final base = await getApplicationSupportDirectory();
      final dir = Directory('${base.path}/vault');
      await dir.create(recursive: true);
      final prefs = await SharedPreferences.getInstance();
      // If the background service outlived the screen, the core is still
      // running and the service is reading its events; join it as it is.
      final serviceRunning = await Background.running;
      final engine = await CordedEngine.open(dir.path, pump: !serviceRunning);
      final store = CordedStore(engine);
      // The first vault_state event may already have gone by.
      store.vaultState = engine.vaultExists() ? 'locked' : 'missing';
      _sub = store.changes.listen((_) {
        _shareRoomTitles();
        notifyListeners();
        _tryPendingOpen();
      });
      Background.listenForTaps(openFromNotification);
      _engine = engine;
      _store = store;
      Background.onEvent((data) {
        if (data is String) engine.deliver(data);
      });
      backgroundMode = serviceRunning;
      if (serviceRunning) {
        Background.tell({'engine': engine.address});
        Background.tell('on_screen');
        Future<void>.delayed(const Duration(seconds: 1), () => _shareRoomTitles(force: true));
        final status = await engine.command({'cmd': 'status'});
        if (status['vault'] == 'unlocked') {
          store.username = '${status['username'] ?? ''}';
          store.vaultState = 'unlocked';
          await store.refresh();
        }
      }
      _wantBackground = prefs.getBool('background_mode') ?? false;
      fingerprintAvailable = await Fingerprint.available();
      fingerprintUnlock = fingerprintAvailable && (prefs.getBool('fingerprint_unlock') ?? false);
      // Chosen earlier, but the service is gone (the phone restarted, say):
      // bring it back once the vault is open.
      store.changes.listen((_) {
        if (_wantBackground && !backgroundMode && store.vaultState == 'unlocked') {
          _wantBackground = false;
          setBackgroundMode(true);
        }
      });
    } catch (e) {
      startError = '$e';
    }
    notifyListeners();
  }

  /// The server whose chats are shown.
  ServerInfo? get server {
    final servers = store.servers;
    if (servers.isEmpty) return null;
    return servers[_selectedServer] ?? servers.values.first;
  }

  void selectServer(int id) {
    _selectedServer = id;
    notifyListeners();
  }

  Future<void> lock() async {
    await engine.command({'cmd': 'lock'});
    store.rooms.clear();
    store.servers.clear();
    notifyListeners();
  }

  @override
  void dispose() {
    _sub?.cancel();
    _store?.dispose();
    _engine?.close();
    super.dispose();
  }
}
