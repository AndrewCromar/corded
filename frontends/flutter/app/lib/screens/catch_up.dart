import 'dart:async';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'chat.dart';
import 'common.dart';

/// Everything unread in one place: each chat with something new, with who
/// wrote and how it starts. Opening one reads it there; it then drops off
/// this list, so the list is worked through from top to bottom.
class CatchUpScreen extends StatefulWidget {
  const CatchUpScreen({super.key, required this.state});
  final AppState state;

  @override
  State<CatchUpScreen> createState() => _CatchUpScreenState();
}

/// Where a chat's unread messages start, as far as this screen needs to know.
typedef _Start = ({String sender, String text, String? thread, String newestId, int unread});

class _CatchUpScreenState extends State<CatchUpScreen> {
  StreamSubscription<void>? _sub;
  final _starts = <String, _Start>{};

  CordedStore get _store => widget.state.store;

  @override
  void initState() {
    super.initState();
    _look();
    widget.state.addListener(_changed);
    widget.state.refreshUnreadThreads();
    _sub = _store.changes.listen((_) {
      _look();
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    widget.state.removeListener(_changed);
    super.dispose();
  }

  void _changed() {
    if (mounted) setState(() {});
  }

  // Threads with replies not seen yet, in this server. One that a chat above
  // already leads into is not listed twice.
  List<Map<String, dynamic>> get _threads {
    final server = widget.state.server;
    return [
      for (final t in widget.state.unreadThreads)
        if (_store.rooms[t['room_id']] != null &&
            (server == null || _store.rooms[t['room_id']]!.serverId == server.id) &&
            _starts[t['room_id']]?.thread != t['root_id'])
          t
    ];
  }

  List<Room> get _unread {
    final server = widget.state.server;
    final rooms = [
      for (final r in _store.rooms.values)
        if (r.unread > 0 && (server == null || r.serverId == server.id)) r
    ]..sort((a, b) => b.lastActivity.compareTo(a.lastActivity));
    return rooms;
  }

  // For each chat with something new, finds the message the unread ones start
  // at. Asked of the core once per change in a chat's unread count.
  Future<void> _look() async {
    for (final room in _unread) {
      final known = _starts[room.id];
      if (known != null && known.unread == room.unread) continue;
      try {
        final r =
            await widget.state.engine.command({'cmd': 'fetch_timeline', 'room_id': room.id, 'limit': 60});
        final events = [
          for (final e in (r['events'] as List? ?? const []))
            if (e is Map) e.cast<String, dynamic>()
        ];
        Map<String, dynamic>? first, newest;
        for (final e in events) {
          if (e['mine'] == true || e['seq'] == null) continue;
          final type = '${e['type']}';
          if (type != 'm.text' && type != 'm.file' && type != 'm.poll' && type != 'm.task') continue;
          newest = e;
          if (e['event_id'] == room.firstUnread) first = e;
        }
        final shown = first ?? newest;
        if (shown == null) continue;
        final message = Message.fromJson(shown);
        _starts[room.id] = (
          sender: message.sender,
          text: message.text.split('\n').first,
          thread: message.threadRoot,
          newestId: '${newest!['event_id']}',
          unread: room.unread,
        );
        if (mounted) setState(() {});
      } on CordedError {
        // Shown without a preview.
      }
    }
  }

  // The chat opens over this list, so that Back comes back here, to the next
  // thing to read.
  void _go(String roomId, String? thread) {
    final navigator = Navigator.of(context);
    navigator.push(MaterialPageRoute(builder: (_) => ChatScreen(state: widget.state, roomId: roomId)));
    if (thread != null) {
      navigator.push(MaterialPageRoute(
          builder: (_) => ChatScreen(state: widget.state, roomId: roomId, threadRoot: thread)));
    }
  }

  void _open(Room room) => _go(room.id, _starts[room.id]?.thread);

  Future<void> _readAll() async {
    for (final roomId in {for (final t in _threads) '${t['room_id']}'}) {
      try {
        await widget.state.engine.command({'cmd': 'mark_thread_read', 'room_id': roomId});
      } on CordedError {
        // Left as it is.
      }
    }
    for (final room in _unread) {
      final newest = _starts[room.id]?.newestId;
      if (newest == null) continue;
      try {
        await widget.state.engine.command({'cmd': 'mark_read', 'room_id': room.id, 'event_id': newest});
      } on CordedError {
        // Left unread; it stays on the list.
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final rooms = _unread;
    final threads = _threads;
    String words(Object? event) {
      if (event is! Map) return '';
      final m = Message.fromJson(event.cast<String, dynamic>());
      return m.text.split('\n').first;
    }

    return Scaffold(
      appBar: AppBar(
        title: const Text('Catch up'),
        actions: [
          if (rooms.isNotEmpty || threads.isNotEmpty)
            TextButton(
              onPressed: () async {
                final sure = await showDialog<bool>(
                  context: context,
                  builder: (context) => AlertDialog(
                    title: const Text('Mark everything as read?'),
                    content: Text('${[
                      if (rooms.isNotEmpty) '${rooms.length} chat${rooms.length == 1 ? '' : 's'}',
                      if (threads.isNotEmpty) '${threads.length} thread${threads.length == 1 ? '' : 's'}',
                    ].join(' and ')} with unread messages.'),
                    actions: [
                      TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
                      FilledButton(
                          onPressed: () => Navigator.pop(context, true), child: const Text('Mark as read')),
                    ],
                  ),
                );
                if (sure == true && context.mounted) await attempt(context, _readAll);
              },
              child: const Text('Mark all read'),
            ),
        ],
      ),
      body: rooms.isEmpty && threads.isEmpty
          ? const Center(child: Text('You are all caught up.'))
          : ListView(children: [
              for (final room in rooms)
                ListTile(
                  leading: Icon(room.kind == 'channel'
                      ? (room.isTaskList ? Icons.checklist : Icons.tag)
                      : room.kind == 'direct'
                          ? Icons.person_outline
                          : Icons.group_outlined),
                  title: Row(children: [
                    Flexible(
                        child: Text(room.kind == 'channel' ? room.title.replaceFirst('#', '') : room.title,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: const TextStyle(fontWeight: FontWeight.bold))),
                    if (_starts[room.id]?.thread != null)
                      Padding(
                        padding: const EdgeInsets.only(left: 8),
                        child: Text('in a thread',
                            style: theme.textTheme.labelSmall?.copyWith(color: theme.colorScheme.primary)),
                      ),
                  ]),
                  subtitle: Text(
                      // A channel marked hidden keeps its words to itself here too.
                      room.nsfw
                          ? 'New messages'
                          : _starts[room.id] == null
                              ? 'New messages'
                              : '${_starts[room.id]!.sender}: ${_starts[room.id]!.text}',
                      maxLines: 2,
                      overflow: TextOverflow.ellipsis),
                  trailing: Badge(label: Text('${room.unread}')),
                  onTap: () => _open(room),
                ),
              for (final t in threads)
                ListTile(
                  leading: const Icon(Icons.forum_outlined),
                  title: Row(children: [
                    Flexible(
                        child: Text(_store.rooms[t['room_id']]!.title.replaceFirst('#', ''),
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: const TextStyle(fontWeight: FontWeight.bold))),
                    Padding(
                      padding: const EdgeInsets.only(left: 8),
                      child: Text('thread',
                          style: theme.textTheme.labelSmall?.copyWith(color: theme.colorScheme.primary)),
                    ),
                  ]),
                  subtitle: Text(
                      _store.rooms[t['room_id']]!.nsfw
                          ? 'New replies'
                          : '${words(t['root'])}\n${(t['first'] as Map?)?['sender_name'] ?? ''}: ${words(t['first'])}',
                      maxLines: 2,
                      overflow: TextOverflow.ellipsis),
                  trailing: Badge(label: Text('${t['count']}')),
                  onTap: () => _go('${t['room_id']}', '${t['root_id']}'),
                ),
            ]),
    );
  }
}
