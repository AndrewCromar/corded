import 'dart:async';
import 'dart:io';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/foundation.dart';
import 'package:characters/characters.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// What the whole app shares: the running core and the lists built from it.
/// The app keeps nothing of its own; the core's vault is the only store.
class AppState extends ChangeNotifier {
  CordedEngine? _engine;
  CordedStore? _store;
  StreamSubscription<void>? _sub;
  String? startError;
  int? _selectedServer;

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

  Future<void> start() async {
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
      final engine = await CordedEngine.open(dir.path);
      final store = CordedStore(engine);
      // The first vault_state event may already have gone by.
      store.vaultState = engine.vaultExists() ? 'locked' : 'missing';
      _sub = store.changes.listen((_) => notifyListeners());
      _engine = engine;
      _store = store;
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
