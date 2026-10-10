import 'desktop_notifier.dart';
import 'updater.dart';
import 'platform.dart';
import 'dart:async';
import 'dart:io';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart'
    show BuildContext, MaterialPageRoute, Navigator, NavigatorState, WidgetBuilder;
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

  /// Signs this device out for good: the vault, with this device's keys and
  /// its copy of every message, is deleted, and the app starts over. Nothing
  /// on any server changes. Unless the person has their recovery key or
  /// another device, the identity is gone.
  Future<void> signOut() async {
    final dir = _vaultDir;
    if (backgroundMode) {
      // The service's task was reading the core's events; without it nothing
      // would answer the commands below, so read them here again.
      await Background.stop();
      await engine.startPump();
    }
    backgroundMode = false;
    _wantBackground = false;
    await Fingerprint.clear();
    fingerprintUnlock = false;
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove('fingerprint_unlock');
    await prefs.remove('background_mode');
    await prefs.remove('muted_rooms');
    mutedRooms = {};
    try {
      await engine.command({'cmd': 'lock'}).timeout(const Duration(seconds: 5));
    } catch (_) {
      // Closing it below is what matters.
    }
    await _sub?.cancel();
    await _store?.dispose();
    await _engine?.close();
    _store = null;
    _engine = null;
    _selectedServer = null;
    notifyListeners();
    if (dir != null && await dir.exists()) await dir.delete(recursive: true);
    await start();
  }

  Directory? _vaultDir;

  /// A newer release, if the daily look found one.
  Update? updateAvailable;

  // Once a day, a quiet look at whether a newer release exists. Nothing is
  // downloaded; the person is told, and updates from Settings when they like.
  Future<void> _lookForUpdate(SharedPreferences prefs) async {
    if (appVersion == 'dev') return; // a build made by hand is left alone
    final today = DateTime.now().toIso8601String().substring(0, 10);
    if (prefs.getString('update_looked') == today) return;
    await Future<void>.delayed(const Duration(seconds: 20));
    try {
      final update = await checkForUpdate();
      await prefs.setString('update_looked', today);
      if (update == null) return;
      updateAvailable = update;
      notifyListeners();
      if (_onScreen && onBanner != null) {
        onBanner!('Corded ${update.tag} is available', 'Settings > Check for updates installs it.', '', null);
      }
    } catch (_) {
      // No network, or GitHub is busy: tomorrow is soon enough.
    }
  }

  // Only in builds made with --dart-define=CORDED_DEV=true, for trying the
  // app where no one can type into it: the environment says who to be, which
  // server to join and which chat to open. Ordinary builds contain none of this.
  static const _devBuild = bool.fromEnvironment('CORDED_DEV');
  void Function(String screen)? onDevScreen;

  Future<void> _devDrive() async {
    final env = Platform.environment;
    final pass = env['CORDED_DEV_PASSPHRASE'];
    if (pass == null) return;
    try {
      if (store.vaultState == 'missing') {
        await engine.createVault(pass, env['CORDED_DEV_USER'] ?? 'dev');
      } else if (store.vaultState == 'locked') {
        await engine.unlock(pass);
      }
      await Future<void>.delayed(const Duration(seconds: 1));
      final server = env['CORDED_DEV_SERVER'];
      if (server != null && store.servers.isEmpty) {
        final parts = server.split(':');
        await engine.command({'cmd': 'connect', 'host': parts[0], 'port': int.parse(parts[1])});
      }
      // CORDED_DEV_UPDATE=1: go through a whole update without anyone clicking.
      if (env['CORDED_DEV_UPDATE'] == '1') {
        final update = await checkForUpdate();
        stderr.writeln('dev update: running $appVersion, found ${update?.tag}');
        if (update != null) {
          final path = await downloadUpdate(engine, update);
          stderr.writeln('dev update: signature ok, installing $path');
          await installUpdate(path);
        }
      }
      final open = env['CORDED_DEV_OPEN'];
      for (var i = 0; open != null && i < 20; i++) {
        await Future<void>.delayed(const Duration(seconds: 1));
        final room = store.rooms.values.where((r) => r.title == open).firstOrNull;
        if (room != null && onOpenChat != null) {
          onOpenChat!(room.id, null);
          break;
        }
      }
      // CORDED_DEV_CLICKS="2:700,300;1:120,200": mouse clicks (1 left, 2 right)
      // at those points of the window, two seconds apart.
      var pointer = 900;
      for (final click in (env['CORDED_DEV_CLICKS'] ?? '').split(';').where((c) => c.contains(':'))) {
        await Future<void>.delayed(const Duration(seconds: 2));
        final xy = click.split(':')[1].split(',');
        final at = Offset(double.parse(xy[0]), double.parse(xy[1]));
        final gestures = GestureBinding.instance;
        pointer++;
        gestures
            .handlePointerEvent(PointerAddedEvent(position: at, kind: PointerDeviceKind.mouse, device: 9));
        gestures.handlePointerEvent(PointerDownEvent(
            position: at,
            kind: PointerDeviceKind.mouse,
            device: 9,
            pointer: pointer,
            buttons: int.parse(click.split(':')[0])));
        gestures.handlePointerEvent(
            PointerUpEvent(position: at, kind: PointerDeviceKind.mouse, device: 9, pointer: pointer));
        gestures
            .handlePointerEvent(PointerRemovedEvent(position: at, kind: PointerDeviceKind.mouse, device: 9));
      }
      // CORDED_DEV_SCREEN=settings: show that screen, for a picture of it.
      if (env['CORDED_DEV_SCREEN'] != null) {
        await Future<void>.delayed(Duration(seconds: int.tryParse(env['CORDED_DEV_WAIT'] ?? '') ?? 3));
        onDevScreen?.call(env['CORDED_DEV_SCREEN']!);
      }
    } catch (e) {
      startError = 'dev drive: $e';
      notifyListeners();
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
      Future<void>.delayed(const Duration(seconds: 2), () async {
        _shareRoomTitles(force: true);
        _shareNotificationOptions();
        try {
          final s = await engine.command({'cmd': 'client_settings'});
          setDoNotDisturb((s['client_settings'] as Map?)?['presence'] == 'dnd');
        } catch (_) {
          // Notifications stay on.
        }
      });
    }
    notifyListeners();
    return null;
  }

  // The background task titles its notifications with the names of the chats.
  String _sharedTitles = '';
  void _shareRoomTitles({bool force = false}) {
    if (!backgroundMode) return;
    final titles = {for (final r in store.rooms.values) r.id: r.title};
    final key = '$titles ${[
      for (final r in store.rooms.values)
        if (r.nsfw) r.id
    ]}';
    if (!force && key == _sharedTitles) return;
    _sharedTitles = key;
    Background.tell({'rooms': titles});
    _shareNotificationOptions();
  }

  /// The chat (and thread) on screen right now, so that a message arriving
  /// there is not announced. Set by the chat screen.
  String? viewingRoom;

  /// The hidden channel the person chose to open, while they are in it.
  String? uncoveredRoom;
  String? viewingThread;
  bool _onScreen = true;
  bool _doNotDisturb = false;

  /// Called with a message that arrived somewhere the person is not looking,
  /// while the app is on screen. The app shows a banner for it.
  void Function(String title, String body, String roomId, String? threadRoot)? onBanner;

  void _maybeBanner(Map<String, dynamic> event) {
    // On a desktop the window may be open but not looked at: then the
    // message is announced by the system, as a phone's service would.
    final elsewhere = isDesktop && !_onScreen;
    if ((!_onScreen && !elsewhere) || _doNotDisturb || onBanner == null) return;
    final n = notificationFor(event, {for (final r in store.rooms.values) r.id: r.title},
        muted: mutedRooms,
        showText: elsewhere ? showMessageText : true,
        me: store.username,
        nsfw: {
          for (final r in store.rooms.values)
            if (r.nsfw) r.id
        });
    if (n == null) return;
    if (elsewhere) {
      DesktopNotifier.show(n);
      return;
    }
    final data = (event['data'] as Map).cast<String, dynamic>();
    final relation = (data['relation'] as Map?) ?? const {};
    final thread = relation['kind'] == 'thread' ? relation['target'] as String? : null;
    // Not for what is already in front of the person.
    if (n.roomId == viewingRoom && thread == viewingThread) return;
    onBanner!(n.title, n.body, n.roomId, thread);
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

  /// Tells the background task whether someone is looking at the app, and the
  /// core too, so that others see this person as online or away.
  /// Readable copies of files exist only while they are being looked at.
  /// This removes the ones handed to other apps (a video player, a PDF
  /// reader) and has the core empty its own view folder.
  Future<void> wipeOpenedFiles() async {
    try {
      final handed = Directory('${(await getTemporaryDirectory()).path}/files');
      if (handed.existsSync()) handed.deleteSync(recursive: true);
    } catch (_) {
      // Nothing there, or still held open by the app showing it.
    }
    if (ready) engine.command({'cmd': 'wipe_views'}).catchError((_) => <String, dynamic>{});
  }

  void setOnScreen(bool onScreen) {
    // Back from whatever app a file was opened in: its readable copy can go.
    if (onScreen && !_onScreen) wipeOpenedFiles();
    _onScreen = onScreen;
    Background.tell(onScreen ? 'on_screen' : 'off_screen');
    if (ready && store.vaultState == 'unlocked') {
      engine.command({'cmd': 'set_active', 'active': onScreen}).catchError((_) => <String, dynamic>{});
    }
  }

  /// Chats whose messages raise no notification on this phone.
  Set<String> mutedRooms = {};

  /// Whether notifications show what a message says, or only that one came.
  bool showMessageText = true;

  bool isMuted(String roomId) => mutedRooms.contains(roomId);

  Future<void> setMuted(String roomId, bool muted) async {
    muted ? mutedRooms.add(roomId) : mutedRooms.remove(roomId);
    notifyListeners();
    _shareNotificationOptions();
    (await SharedPreferences.getInstance()).setStringList('muted_rooms', mutedRooms.toList());
  }

  /// Threads holding replies not seen yet, as the core lists them: each with
  /// `room_id`, `root_id`, `count`, and the messages `root` and `first`. A
  /// thread keeps its own place; reading its channel does not open it.
  List<Map<String, dynamic>> unreadThreads = const [];
  Timer? _threadsTimer;

  Future<void> refreshUnreadThreads() async {
    if (_store == null || store.vaultState != 'unlocked') return;
    try {
      final r = await engine.command({'cmd': 'unread_threads'});
      final now = [
        for (final t in (r['threads'] as List? ?? const []))
          if (t is Map) t.cast<String, dynamic>()
      ];
      String mark(List<Map<String, dynamic>> l) => l.map((t) => '${t['root_id']}:${t['count']}').join(',');
      if (mark(now) == mark(unreadThreads)) return;
      unreadThreads = now;
      notifyListeners();
    } on CordedError {
      // Asked again at the next change.
    }
  }

  /// On a wide desktop window, the part beside the list of chats; a page
  /// opened from the list goes there instead of covering everything.
  NavigatorState? Function()? paneNavigator;

  void openPage(BuildContext context, WidgetBuilder page) =>
      (paneNavigator?.call() ?? Navigator.of(context)).push(MaterialPageRoute(builder: page));

  /// The groups in the list of chats that are not as they start out: opened
  /// when they start closed, or closed when they start open.
  Set<String> _sectionsFlipped = {};

  bool sectionOpen(String id, {required bool byDefault}) =>
      _sectionsFlipped.contains(id) ? !byDefault : byDefault;

  Future<void> setSectionOpen(String id, bool open, {required bool byDefault}) async {
    open == byDefault ? _sectionsFlipped.remove(id) : _sectionsFlipped.add(id);
    (await SharedPreferences.getInstance()).setStringList('sections_flipped', _sectionsFlipped.toList());
  }

  /// Whether a link you send gets a preview card, which this device fetches.
  bool linkPreviews = true;

  Future<void> setLinkPreviews(bool on) async {
    linkPreviews = on;
    notifyListeners();
    (await SharedPreferences.getInstance()).setBool('link_previews', on);
  }

  Future<void> setShowMessageText(bool show) async {
    showMessageText = show;
    notifyListeners();
    _shareNotificationOptions();
    (await SharedPreferences.getInstance()).setBool('show_message_text', show);
  }

  void _shareNotificationOptions() => Background.tell({
        'muted': mutedRooms.toList(),
        'show_text': showMessageText,
        'me': store.username,
        'nsfw': [
          for (final r in store.rooms.values)
            if (r.nsfw) r.id
        ],
      });

  /// With do not disturb on, this phone shows no message notifications.
  void setDoNotDisturb(bool on) {
    _doNotDisturb = on;
    Background.tell(on ? 'dnd_on' : 'dnd_off');
  }

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
      _vaultDir = dir;
      final prefs = await SharedPreferences.getInstance();
      _sectionsFlipped = (prefs.getStringList('sections_flipped') ?? const []).toSet();
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
      DesktopNotifier.init(openFromNotification);
      engine.events.listen((event) {
        if (event['event'] == 'event_received') _maybeBanner(event);
        // What a server tells the people who run it: a setting changed, an
        // update went through, file space is running out.
        if (event['event'] == 'server_notice' && _onScreen && onBanner != null) {
          final from = store.servers[(event['server_id'] as num?)?.toInt()]?.name ?? '';
          onBanner!(from.isEmpty ? 'Server' : from, '${event['message'] ?? ''}', '', null);
        }
      });
      _engine = engine;
      _store = store;
      // Whatever changes may have changed which threads hold something new.
      store.changes.listen((_) {
        _threadsTimer?.cancel();
        _threadsTimer = Timer(const Duration(milliseconds: 400), refreshUnreadThreads);
      });
      Background.onEvent((data) {
        if (data is String) engine.deliver(data);
      });
      backgroundMode = serviceRunning;
      if (serviceRunning) {
        Background.tell({'engine': engine.address});
        Background.tell('on_screen');
        Future<void>.delayed(const Duration(seconds: 1), () => _shareRoomTitles(force: true));
      }
      // The core can outlive the screen (the service kept it, or Android only
      // threw the screen away): if its vault is still open, carry on from there.
      if (store.vaultState == 'locked') {
        final status = await engine.command({'cmd': 'status'}).timeout(const Duration(seconds: 5));
        if (status['vault'] == 'unlocked') {
          store.username = '${status['username'] ?? ''}';
          store.vaultState = 'unlocked';
          await store.refresh();
        }
      }
      _wantBackground = prefs.getBool('background_mode') ?? false;
      mutedRooms = (prefs.getStringList('muted_rooms') ?? const []).toSet();
      showMessageText = prefs.getBool('show_message_text') ?? true;
      linkPreviews = prefs.getBool('link_previews') ?? true;
      if (serviceRunning) {
        Future<void>.delayed(const Duration(seconds: 1), _shareNotificationOptions);
      }
      unawaited(wipeOpenedFiles()); // anything readable left by an earlier run
      if (_devBuild) unawaited(_devDrive());
      unawaited(_lookForUpdate(prefs));
      // An update that did not go in last time says so now, in plain words.
      final failure = lastUpdateFailure();
      if (failure != null) {
        Future<void>.delayed(const Duration(seconds: 3), () {
          onBanner?.call('The update was not installed',
              '$failure. Corded is unchanged; try again from Settings.', '', null);
        });
      }
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
    await wipeOpenedFiles();
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
