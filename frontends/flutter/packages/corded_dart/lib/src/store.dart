import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'engine.dart';
import 'mentions.dart';
import 'models.dart';

/// Keeps the lists a screen draws up to date from the core's events.
///
/// Listen to [changes] and redraw; read the fields. Nothing here is saved: the
/// core's vault is the only store, and this is rebuilt from it at each start.
class CordedStore {
  CordedStore(this.engine) {
    _sub = engine.events.listen(_onEvent);
  }

  final CordedEngine engine;
  late final StreamSubscription<Map<String, dynamic>> _sub;
  final _changes = StreamController<void>.broadcast();

  /// Fires after anything below changed.
  Stream<void> get changes => _changes.stream;

  String vaultState = 'unknown'; // missing, locked, unlocked
  String username = '';
  final Map<int, ServerInfo> servers = {};
  final Map<String, Room> rooms = {};
  final Map<String, List<Message>> _messages = {};
  final Map<String, Map<String, Map<String, Set<String>>>> _reactions = {}; // room -> message -> key -> ids
  final Map<String, Map<String, Map<String, String>>> _myReactions = {}; // room -> message -> key -> my reaction's id
  final Map<String, Map<String, DateTime>> _typing = {}; // room -> name -> until
  final Map<String, Map<String, String>> _readUpTo = {}; // room -> name -> message id
  final Map<int, Map<String, String>> _presence = {}; // server -> person -> online, away or dnd
  final Map<String, Map<String, Map<String, (int, List<int>)>>> _votes = {}; // room -> poll -> voter -> (when, choices)
  final Map<String, Map<String, List<int>>> _myVotes = {}; // room -> poll -> this person's choices
  final Map<String, Uint8List?> _pictures = {}; // person -> their picture; null if they have none
  final Set<String> _loaded = {};
  final Map<String, String?> _oldest = {}; // room -> where the next older page starts; null = none left
  String notice = '';

  List<Room> roomsOf(int serverId) {
    final list = rooms.values.where((r) => r.serverId == serverId).toList();
    int rank(Room r) => r.kind == 'channel' ? 0 : 1;
    list.sort((a, b) {
      final byKind = rank(a).compareTo(rank(b));
      if (byKind != 0) return byKind;
      if (a.kind == 'channel') return a.title.compareTo(b.title);
      return b.lastActivity.compareTo(a.lastActivity);
    });
    return list;
  }

  /// The conversation as shown: everything except replies inside threads.
  List<Message> messages(String roomId) =>
      [for (final m in _all(roomId)) if (m.threadRoot == null) m];

  /// The replies in the thread started by [rootId], oldest first.
  List<Message> thread(String roomId, String rootId) =>
      [for (final m in _all(roomId)) if (m.threadRoot == rootId) m];

  /// How many replies a message's thread has.
  int threadCount(String roomId, Message root) {
    final loaded = thread(roomId, root.id).length;
    return loaded > root.threadCount ? loaded : root.threadCount;
  }

  Message? message(String roomId, String id) => _all(roomId).where((m) => m.id == id).firstOrNull;

  List<Message> _all(String roomId) => _messages[roomId] ?? const [];

  /// Loads every reply of a thread, including ones older than the pages shown.
  Future<void> openThread(String roomId, String rootId) async {
    try {
      final r = await engine.command({'cmd': 'fetch_thread', 'room_id': roomId, 'event_id': rootId});
      for (final j in (r['thread'] as List? ?? const [])) {
        _applyMessage((j as Map).cast<String, dynamic>());
      }
    } on CordedError {
      // Shown with whatever is already loaded.
    }
    await markRead(roomId);
    _changed();
  }

  // Server order first. A message still being sent has no place yet and goes
  // last, in the order it was written. Clocks on different devices disagree,
  // so the time a sender wrote on a message is never used to order it.
  static int compareMessages(Message a, Message b) {
    final sa = a.seq, sb = b.seq;
    if (sa != null && sb != null) return sa.compareTo(sb);
    if (sa != null) return -1;
    if (sb != null) return 1;
    return a.timestamp.compareTo(b.timestamp);
  }

  /// Reactions on a message: emoji (or word) to how many people sent it.
  Map<String, int> reactions(String roomId, String messageId) {
    final byKey = _reactions[roomId]?[messageId] ?? const {};
    return {
      for (final e in byKey.entries)
        if (e.value.isNotEmpty) e.key: e.value.length
    };
  }

  /// The id of this person's own reaction with [key] on a message, if they
  /// made one. Deleting that event takes the reaction back.
  String? myReaction(String roomId, String messageId, String key) {
    final id = _myReactions[roomId]?[messageId]?[key];
    final live = _reactions[roomId]?[messageId]?[key]?.contains(id) ?? false;
    return live ? id : null;
  }

  /// Adds this person's reaction, or takes it back if they already made it.
  Future<void> toggleReaction(String roomId, String messageId, String key) async {
    final mine = myReaction(roomId, messageId, key);
    if (mine != null) {
      await engine.command({'cmd': 'delete_event', 'room_id': roomId, 'event_id': mine});
    } else {
      await engine.command({
        'cmd': 'send_event',
        'room_id': roomId,
        'type': 'm.reaction',
        'content': {'key': key},
        'relation': {'kind': 'annotation', 'target': messageId, 'key': key},
      });
    }
  }

  /// Someone's profile as this device knows it; yours when [userId] is null.
  Future<Profile> profile([String? userId]) async {
    final r = await engine.command({'cmd': 'get_profile', if (userId != null) 'user_id': userId});
    return Profile.fromJson(((r['profile'] as Map?) ?? const {}).cast<String, dynamic>());
  }

  /// Someone's profile picture, if this device has it. The first call for a
  /// person looks it up in the vault and reports through [changes].
  Uint8List? picture(String userId) {
    if (_pictures.containsKey(userId)) return _pictures[userId];
    _pictures[userId] = null;
    engine.command({'cmd': 'get_profile', 'user_id': userId}).then((r) {
      _setPicture(userId, ((r['profile'] as Map?) ?? const {})['picture']);
      _changed();
    }).catchError((_) {});
    return null;
  }

  void _setPicture(String userId, Object? base64Picture) {
    Uint8List? bytes;
    if (base64Picture is String && base64Picture.isNotEmpty) {
      try {
        bytes = base64Decode(base64Picture);
      } catch (_) {
        bytes = null;
      }
    }
    _pictures[userId] = bytes;
  }

  /// How present someone is on a server: online, away, dnd or offline.
  String presence(int serverId, String userId) => _presence[serverId]?[userId] ?? 'offline';

  /// How many people chose each option of a poll, by option number.
  Map<int, int> pollCounts(String roomId, String pollId) {
    final counts = <int, int>{};
    for (final vote in (_votes[roomId]?[pollId] ?? const <String, (int, List<int>)>{}).values) {
      for (final choice in vote.$2.toSet()) {
        counts[choice] = (counts[choice] ?? 0) + 1;
      }
    }
    return counts;
  }

  /// How many people have voted in a poll.
  int pollVoters(String roomId, String pollId) =>
      (_votes[roomId]?[pollId] ?? const <String, (int, List<int>)>{}).values.where((v) => v.$2.isNotEmpty).length;

  /// What this person chose in a poll.
  List<int> myPollChoices(String roomId, String pollId) => _myVotes[roomId]?[pollId] ?? const [];

  /// Votes, or changes a vote. One choice replaces the last unless the poll
  /// allows several; choosing what is already chosen takes it back.
  Future<void> votePoll(Message poll, int option) async {
    final mine = {...myPollChoices(poll.roomId, poll.id)};
    if (mine.contains(option)) {
      mine.remove(option);
    } else {
      if (!poll.pollMultiple) mine.clear();
      mine.add(option);
    }
    await engine.command({
      'cmd': 'send_event',
      'room_id': poll.roomId,
      'type': 'm.poll.vote',
      'content': {'choices': mine.toList()..sort()},
      'relation': {'kind': 'annotation', 'target': poll.id, 'key': 'vote'},
    });
  }

  /// Starts a poll. In a thread when [thread] is given.
  Future<void> createPoll(String roomId, String question, List<String> options,
      {bool multiple = false, String? thread}) async {
    await engine.command({
      'cmd': 'send_event',
      'room_id': roomId,
      'type': 'm.poll',
      // What clients that do not know polls show instead.
      'fallback_text': 'Poll: $question (${options.join(' / ')})',
      'content': {'question': question, 'options': options, 'multiple': multiple},
      if (thread != null) 'relation': {'kind': 'thread', 'target': thread},
    });
  }

  /// Display names of those typing in a room right now.
  List<String> typing(String roomId) {
    final now = DateTime.now();
    return [
      for (final e in (_typing[roomId] ?? const <String, DateTime>{}).entries)
        if (e.value.isAfter(now)) e.key
    ];
  }

  /// Display names of those whose newest read message is this one.
  List<String> readBy(String roomId, String messageId) => [
        for (final e in (_readUpTo[roomId] ?? const <String, String>{}).entries)
          if (e.value == messageId) e.key
      ];

  bool hasOlder(String roomId) => _oldest[roomId] != null;

  /// Loads the newest messages of a room the first time it is opened.
  Future<void> open(String roomId) async {
    if (_loaded.add(roomId)) {
      await _loadPage(roomId, null);
      try {
        final r = await engine.command({'cmd': 'fetch_receipts', 'room_id': roomId});
        for (final j in (r['receipts'] as List? ?? const [])) {
          _applyReceipt(roomId, (j as Map).cast<String, dynamic>());
        }
      } on CordedError {
        // Receipts are a nicety; a chat opens without them.
      }
    }
    await markRead(roomId);
    _changed();
  }

  /// Loads the page of messages before the oldest one shown.
  Future<void> loadOlder(String roomId) async {
    final before = _oldest[roomId];
    if (before != null) await _loadPage(roomId, before);
    _changed();
  }

  /// Tells the others (unless turned off) that everything shown has been read.
  Future<void> markRead(String roomId) async {
    final list = _all(roomId);
    for (var i = list.length - 1; i >= 0; i--) {
      if (list[i].mine || list[i].seq == null) continue;
      try {
        await engine.command({'cmd': 'mark_read', 'room_id': roomId, 'event_id': list[i].id});
      } on CordedError {
        // Not yet confirmed by the server; the next message will carry it.
      }
      return;
    }
  }

  Future<void> refresh() async {
    try {
      final s = await engine.command({'cmd': 'list_servers'});
      for (final j in (s['servers'] as List? ?? const [])) {
        _applyServer((j as Map).cast<String, dynamic>());
      }
      final r = await engine.command({'cmd': 'list_rooms'});
      for (final j in (r['rooms'] as List? ?? const [])) {
        _applyRoom((j as Map).cast<String, dynamic>());
      }
    } on CordedError {
      // Locked or not set up yet: nothing to list.
    }
    _changed();
  }

  Future<void> dispose() async {
    await _sub.cancel();
    await _changes.close();
  }

  // ---- internals

  void _changed() {
    if (!_changes.isClosed) _changes.add(null);
  }

  Future<void> _loadPage(String roomId, String? before) async {
    final Map<String, dynamic> page;
    try {
      page = await engine.command({
        'cmd': 'fetch_timeline',
        'room_id': roomId,
        'limit': 60,
        if (before != null) 'before': before,
      });
    } on CordedError {
      return;
    }
    for (final j in (page['events'] as List? ?? const [])) {
      _applyMessage((j as Map).cast<String, dynamic>());
    }
    _oldest[roomId] = page['more'] == true ? page['oldest'] as String? : null;
  }

  void _applyServer(Map<String, dynamic> j) {
    final id = (j['server_id'] as num?)?.toInt();
    if (id == null || id == 0) return;
    servers.putIfAbsent(id, () => ServerInfo(id)).apply(j);
  }

  Room _applyRoom(Map<String, dynamic> j) {
    final id = j['room_id'] as String;
    return rooms.putIfAbsent(id, () => Room(id))..apply(j);
  }

  void _applyReceipt(String roomId, Map<String, dynamic> j) {
    if (j['username'] == username || j['display_name'] == null) return;
    (_readUpTo[roomId] ??= {})['${j['display_name']}'] = '${j['event_id']}';
  }

  // Returns true if this was a message not seen before.
  bool _applyMessage(Map<String, dynamic> j) {
    final roomId = j['room_id'] as String? ?? '';
    final id = j['event_id'] as String? ?? '';
    final type = j['type'] as String? ?? '';
    final relation = (j['relation'] as Map?) ?? const {};
    if (type == 'm.reaction') {
      final target = relation['target'] as String?;
      if (target == null) return false;
      final key = '${relation['key'] ?? (j['content'] as Map?)?['key'] ?? '?'}';
      if (j['status'] == 'redacted') return false;
      if (j['mine'] == true) ((_myReactions[roomId] ??= {})[target] ??= {})[key] = id;
      return (((_reactions[roomId] ??= {})[target] ??= {})[key] ??= {}).add(id);
    }
    if (type == 'm.poll.vote') {
      // A person's newest vote on a poll replaces their earlier one.
      final target = relation['target'] as String?;
      final voter = j['sender_user'] as String? ?? '';
      if (target == null || voter.isEmpty || j['status'] == 'redacted') return false;
      final at = (j['seq'] as num?)?.toInt() ?? (j['origin_ts'] as num?)?.toInt() ?? 0;
      final votes = (_votes[roomId] ??= {})[target] ??= {};
      final earlier = votes[voter];
      if (earlier != null && earlier.$1 > at) return false;
      final choices = [
        for (final c in ((j['content'] as Map?)?['choices'] as List? ?? const []))
          if (c is num) c.toInt()
      ];
      votes[voter] = (at, choices);
      if (j['mine'] == true) ((_myVotes[roomId] ??= {})[target] = choices);
      return false;
    }
    if (type != 'm.text' && type != 'm.poll') return false; // edits, deletions and room changes arrive as updates
    final list = _messages[roomId] ??= [];
    for (final m in list) {
      if (m.id == id) {
        m.apply(j);
        m.mentionsMe = !m.mine && mentionsUser(m.body, username);
        list.sort(compareMessages);
        return false;
      }
    }
    final message = Message.fromJson(j);
    message.mentionsMe = !message.mine && mentionsUser(message.body, username);
    // Pages of older messages arrive later, so find its place each time.
    var at = list.length;
    while (at > 0 && compareMessages(list[at - 1], message) > 0) {
      at--;
    }
    list.insert(at, message);
    final room = rooms[roomId];
    if (room != null && message.timestamp > room.lastActivity) room.lastActivity = message.timestamp;
    return true;
  }

  void _onEvent(Map<String, dynamic> e) {
    switch (e['event']) {
      case 'vault_state':
        vaultState = e['state'] as String? ?? vaultState;
        username = e['username'] as String? ?? username;
        if (vaultState == 'unlocked') refresh();
      case 'connection_state':
        final id = (e['server_id'] as num?)?.toInt();
        if (id != null && id != 0) {
          servers.putIfAbsent(id, () => ServerInfo(id)).connection = e['state'] as String? ?? '';
        }
      case 'server_info':
        _applyServer(e);
      case 'room_updated':
        _applyRoom((e['room'] as Map).cast<String, dynamic>());
      case 'profile_updated':
        _setPicture('${e['user_id']}', ((e['profile'] as Map?) ?? const {})['picture']);
      case 'presence':
        final server = (e['server_id'] as num?)?.toInt() ?? 0;
        final status = '${e['status']}';
        if (status == 'offline') {
          _presence[server]?.remove(e['user_id']);
        } else {
          (_presence[server] ??= {})['${e['user_id']}'] = status;
        }
      case 'presence_reset':
        _presence.remove((e['server_id'] as num?)?.toInt() ?? 0);
      case 'server_removed':
        final id = (e['server_id'] as num?)?.toInt();
        servers.remove(id);
        for (final room in rooms.values.where((r) => r.serverId == id).toList()) {
          rooms.remove(room.id);
          _messages.remove(room.id);
        }
      case 'room_removed':
        final id = e['room_id'] as String?;
        rooms.remove(id);
        _messages.remove(id);
      case 'event_received':
        final data = (e['data'] as Map).cast<String, dynamic>();
        final roomId = data['room_id'] as String? ?? '';
        _applyMessage(data);
        _typing[roomId]?.remove(data['sender_name']);
        final unread = (e['unread'] as num?)?.toInt();
        if (unread != null) rooms[roomId]?.unread = unread;
      case 'event_updated':
        final data = (e['data'] as Map).cast<String, dynamic>();
        final roomId = data['room_id'] as String? ?? '';
        final id = data['event_id'];
        for (final m in _all(roomId)) {
          if (m.id == id) {
            m.apply(data);
            m.mentionsMe = !m.mine && mentionsUser(m.body, username);
          }
        }
        // A deleted reaction no longer counts.
        for (final byKey in (_reactions[roomId] ?? const <String, Map<String, Set<String>>>{}).values) {
          for (final ids in byKey.values) {
            ids.remove(id);
          }
        }
      case 'event_send_status':
        final status = e['status'] == 'sent' ? 'ok' : '${e['status']}';
        final list = _messages[e['room_id'] as String? ?? ''] ?? [];
        for (final m in list) {
          if (m.id == e['event_id']) {
            m.status = status;
            m.seq = (e['event_seq'] as num?)?.toInt() ?? m.seq;
          }
        }
        list.sort(compareMessages); // the server has now given it a place
        if (e['status'] == 'failed') notice = 'Could not send: ${e['message'] ?? ''}';
      case 'event_expired':
        _messages[e['room_id'] as String? ?? '']?.removeWhere((m) => m.id == e['event_id']);
      case 'typing':
        (_typing[e['room_id'] as String? ?? ''] ??= {})['${e['display_name'] ?? 'Someone'}'] =
            DateTime.now().add(const Duration(seconds: 5));
        // Redraw once more when the notice lapses.
        Timer(const Duration(seconds: 5, milliseconds: 100), _changed);
      case 'receipt':
        _applyReceipt(e['room_id'] as String? ?? '', e);
      case 'server_notice':
        notice = e['message'] as String? ?? '';
      case 'command_result':
        return; // answered through the command's future
    }
    _changed();
  }
}
