import 'dart:async';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_state.dart';
import 'common.dart';
import 'linked_text.dart';

/// One conversation: its messages and the box for writing a new one. With
/// [threadRoot] it shows one thread instead: the message that started it and
/// the replies under it.
class ChatScreen extends StatefulWidget {
  const ChatScreen({super.key, required this.state, required this.roomId, this.threadRoot});
  final AppState state;
  final String roomId;
  final String? threadRoot;

  @override
  State<ChatScreen> createState() => _ChatScreenState();
}

class _ChatScreenState extends State<ChatScreen> {
  final _input = TextEditingController();
  final _scroll = ScrollController();
  StreamSubscription<void>? _sub;
  Message? _replyingTo;
  Message? _editing;
  int _seen = 0;
  bool _loadingOlder = false;

  CordedStore get _store => widget.state.store;
  String get _room => widget.roomId;
  String? get _thread => widget.threadRoot;

  // What this screen lists: the conversation, or one thread with its first
  // message on top.
  List<Message> get _shown {
    final thread = _thread;
    if (thread == null) return _store.messages(_room);
    final root = _store.message(_room, thread);
    return [if (root != null) root, ..._store.thread(_room, thread)];
  }

  void _openThread(Message root) {
    Navigator.push(
        context,
        MaterialPageRoute(
            builder: (_) => ChatScreen(state: widget.state, roomId: _room, threadRoot: root.id)));
  }

  @override
  void initState() {
    super.initState();
    if (_thread == null) {
      _store.open(_room);
    } else {
      _store.openThread(_room, _thread!);
    }
    // While this screen is up, whatever arrives has been read.
    _sub = _store.changes.listen((_) {
      final count = _shown.length;
      if (count != _seen) {
        _seen = count;
        _store.markRead(_room);
      }
      if (mounted) setState(() {});
    });
    _scroll.addListener(() {
      // The list is drawn newest-first, so its far end is the oldest message.
      if (_scroll.position.pixels > _scroll.position.maxScrollExtent - 200) _loadOlder();
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    _input.dispose();
    _scroll.dispose();
    super.dispose();
  }

  Future<void> _loadOlder() async {
    if (_thread != null || _loadingOlder || !_store.hasOlder(_room)) return;
    _loadingOlder = true;
    await _store.loadOlder(_room);
    _loadingOlder = false;
  }

  Future<void> _send() async {
    final text = _input.text.trim();
    if (text.isEmpty) return;
    final editing = _editing, replyingTo = _replyingTo;
    _input.clear();
    setState(() {
      _editing = null;
      _replyingTo = null;
    });
    await attempt(context, () async {
      if (editing != null) {
        await widget.state.engine
            .command({'cmd': 'edit_event', 'room_id': _room, 'event_id': editing.id, 'body': text});
      } else {
        await widget.state.engine.command({
          'cmd': 'send_text',
          'room_id': _room,
          'body': text,
          // A message has one relation: inside a thread it belongs to the thread.
          if (_thread != null) 'thread': _thread else if (replyingTo != null) 'reply_to': replyingTo.id,
        });
      }
    });
  }

  // Reacting again with the same emoji takes the reaction back.
  Future<void> _react(Message m, String key) =>
      attempt(context, () => _store.toggleReaction(_room, m.id, key));

  // Any emoji, typed or picked from the phone's own keyboard.
  Future<void> _reactWithOther(Message m) async {
    final controller = TextEditingController();
    final text = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('React with'),
        content: TextField(
          controller: controller,
          autofocus: true,
          textAlign: TextAlign.center,
          style: const TextStyle(fontSize: 28),
          decoration: const InputDecoration(hintText: 'Pick an emoji on your keyboard'),
          onSubmitted: (v) => Navigator.pop(context, v),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, controller.text), child: const Text('React')),
        ],
      ),
    );
    final picked = AppState.parseReactions(text ?? '');
    if (picked.isNotEmpty) await _react(m, picked.first);
  }

  void _showActions(Message m) {
    final canManage = widget.state.server?.permissions.contains('manage_messages') ?? false;
    showModalBottomSheet<void>(
      context: context,
      showDragHandle: true,
      builder: (sheet) => SafeArea(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          Row(mainAxisAlignment: MainAxisAlignment.spaceEvenly, children: [
            for (final key in widget.state.reactionBar)
              Flexible(
                child: IconButton(
                  icon: Text(key, style: const TextStyle(fontSize: 24)),
                  onPressed: () {
                    Navigator.pop(sheet);
                    _react(m, key);
                  },
                ),
              ),
            Flexible(
              child: IconButton(
                tooltip: 'Another emoji',
                icon: const Icon(Icons.add_circle_outline),
                onPressed: () {
                  Navigator.pop(sheet);
                  _reactWithOther(m);
                },
              ),
            ),
          ]),
          if (_thread == null) ...[
            ListTile(
              leading: const Icon(Icons.reply),
              title: const Text('Reply'),
              onTap: () {
                Navigator.pop(sheet);
                setState(() {
                  _replyingTo = m;
                  _editing = null;
                });
              },
            ),
            ListTile(
              leading: const Icon(Icons.forum_outlined),
              title: Text(_store.threadCount(_room, m) > 0 ? 'Open thread' : 'Start a thread'),
              onTap: () {
                Navigator.pop(sheet);
                _openThread(m);
              },
            ),
          ],
          ListTile(
            leading: const Icon(Icons.copy),
            title: const Text('Copy text'),
            onTap: () {
              Navigator.pop(sheet);
              Clipboard.setData(ClipboardData(text: m.body));
            },
          ),
          if (m.mine)
            ListTile(
              leading: const Icon(Icons.edit_outlined),
              title: const Text('Edit'),
              onTap: () {
                Navigator.pop(sheet);
                setState(() {
                  _editing = m;
                  _replyingTo = null;
                  _input.text = m.body;
                });
              },
            ),
          if (m.mine || canManage)
            ListTile(
              leading: const Icon(Icons.delete_outline),
              title: const Text('Delete'),
              onTap: () {
                Navigator.pop(sheet);
                attempt(
                    context,
                    () => widget.state.engine
                        .command({'cmd': 'delete_event', 'room_id': _room, 'event_id': m.id}));
              },
            ),
        ]),
      ),
    );
  }

  static String _time(int ms) {
    final t = DateTime.fromMillisecondsSinceEpoch(ms);
    final now = DateTime.now();
    final hm = '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';
    final sameDay = t.year == now.year && t.month == now.month && t.day == now.day;
    return sameDay ? hm : '${t.year}-${t.month.toString().padLeft(2, '0')}-${t.day.toString().padLeft(2, '0')} $hm';
  }

  Widget _bubble(Message m, List<Message> all) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
    final gone = m.status == 'redacted' || m.status == 'undecryptable';
    final quoted = m.replyTo == null ? null : all.where((o) => o.id == m.replyTo).firstOrNull;
    final reactions = _store.reactions(_room, m.id);
    final readers = _store.readBy(_room, m.id);
    final foreground = m.mine ? scheme.onPrimaryContainer : scheme.onSurface;
    final note = <String>[
      _time(m.timestamp),
      if (m.edited) 'edited',
      if (m.mine && m.status == 'pending') 'sending',
      if (m.mine && m.status == 'failed') 'not sent',
    ].join(' · ');

    return Align(
      alignment: m.mine ? Alignment.centerRight : Alignment.centerLeft,
      child: ConstrainedBox(
        constraints: BoxConstraints(maxWidth: MediaQuery.sizeOf(context).width * 0.8),
        child: Column(
          crossAxisAlignment: m.mine ? CrossAxisAlignment.end : CrossAxisAlignment.start,
          children: [
            GestureDetector(
              onLongPress: gone ? null : () => _showActions(m),
              onDoubleTap: gone ? null : () => _react(m, widget.state.reactionBar.first),
              onSecondaryTap: gone ? null : () => _showActions(m),
              child: Container(
                margin: const EdgeInsets.fromLTRB(12, 4, 12, 0),
                padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                decoration: BoxDecoration(
                  color: m.mine ? scheme.primaryContainer : scheme.surfaceContainerHighest,
                  borderRadius: BorderRadius.circular(14),
                ),
                child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  if (!m.mine)
                    Text(m.sender,
                        style: theme.textTheme.labelMedium
                            ?.copyWith(color: scheme.primary, fontWeight: FontWeight.bold)),
                  if (m.replyTo != null)
                    Container(
                      margin: const EdgeInsets.only(top: 2, bottom: 4),
                      padding: const EdgeInsets.only(left: 8),
                      decoration:
                          BoxDecoration(border: Border(left: BorderSide(color: scheme.primary, width: 3))),
                      child: Text(
                        quoted == null ? 'A message not shown here' : '${quoted.sender}: ${quoted.text}',
                        maxLines: 2,
                        overflow: TextOverflow.ellipsis,
                        style: theme.textTheme.bodySmall?.copyWith(color: foreground.withValues(alpha: 0.75)),
                      ),
                    ),
                  LinkedText(
                      gone ? m.text : m.body,
                      style: theme.textTheme.bodyLarge?.copyWith(
                          color: foreground.withValues(alpha: gone ? 0.6 : 1),
                          fontStyle: gone ? FontStyle.italic : null),
                      linkColor: scheme.primary),
                  const SizedBox(height: 2),
                  Text(note,
                      style: theme.textTheme.labelSmall?.copyWith(
                          color: m.status == 'failed' ? scheme.error : foreground.withValues(alpha: 0.6))),
                ]),
              ),
            ),
            if (reactions.isNotEmpty)
              Padding(
                padding: const EdgeInsets.fromLTRB(12, 2, 12, 0),
                child: Wrap(spacing: 4, children: [
                  for (final e in reactions.entries)
                    // Filled when one of them is yours; tap to add or take back.
                    FilterChip(
                      visualDensity: VisualDensity.compact,
                      showCheckmark: false,
                      selected: _store.myReaction(_room, m.id, e.key) != null,
                      label: Text('${e.key} ${e.value}'),
                      onSelected: (_) => _react(m, e.key),
                    ),
                ]),
              ),
            if (_thread == null && _store.threadCount(_room, m) > 0)
              // A thread hangs off its first message as a tappable card.
              Padding(
                padding: const EdgeInsets.fromLTRB(24, 4, 12, 2),
                child: Material(
                  color: scheme.secondaryContainer,
                  borderRadius: BorderRadius.circular(12),
                  clipBehavior: Clip.antiAlias,
                  child: InkWell(
                    onTap: () => _openThread(m),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                      child: Row(mainAxisSize: MainAxisSize.min, children: [
                        Icon(Icons.forum, size: 18, color: scheme.onSecondaryContainer),
                        const SizedBox(width: 8),
                        Flexible(
                          child: Text(() {
                            final n = _store.threadCount(_room, m);
                            final replies = _store.thread(_room, m.id);
                            final last = replies.isEmpty ? '' : '  ·  last from ${replies.last.mine ? 'you' : replies.last.sender}';
                            return '${n == 1 ? 'Thread: 1 reply' : 'Thread: $n replies'}$last';
                          }(),
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: theme.textTheme.labelLarge?.copyWith(color: scheme.onSecondaryContainer)),
                        ),
                        Icon(Icons.chevron_right, size: 20, color: scheme.onSecondaryContainer),
                      ]),
                    ),
                  ),
                ),
              ),
            if (readers.isNotEmpty)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 2, 16, 0),
                child: Text('Read by ${readers.join(', ')}', style: theme.textTheme.labelSmall),
              ),
          ],
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final room = _store.rooms[_room];
    final messages = _shown;
    final typing = _store.typing(_room);
    final banner = _editing != null
        ? 'Editing your message'
        : _replyingTo != null
            ? 'Replying to ${_replyingTo!.sender}: ${_replyingTo!.text}'
            : null;

    return Scaffold(
      appBar: AppBar(
        title: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text(_thread != null ? 'Thread' : room?.title ?? 'Chat', maxLines: 1, overflow: TextOverflow.ellipsis),
          if (_thread != null)
            Text(room?.title ?? '', style: theme.textTheme.labelSmall)
          else if (room != null && room.kind != 'direct')
            Text('${room.members.length} members', style: theme.textTheme.labelSmall),
        ]),
      ),
      body: SafeArea(
        child: Column(children: [
          Expanded(
            child: messages.isEmpty
                ? Center(child: Text(_thread != null ? 'This thread is empty.' : 'No messages yet. Say hello.'))
                : ListView.builder(
                    controller: _scroll,
                    reverse: true,
                    padding: const EdgeInsets.symmetric(vertical: 8),
                    itemCount: messages.length,
                    itemBuilder: (context, i) => _bubble(messages[messages.length - 1 - i], messages),
                  ),
          ),
          if (typing.isNotEmpty)
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 4),
              child: Align(
                alignment: Alignment.centerLeft,
                child: Text('${typing.join(', ')} ${typing.length == 1 ? 'is' : 'are'} typing...',
                    style: theme.textTheme.labelMedium),
              ),
            ),
          if (banner != null)
            Material(
              color: theme.colorScheme.secondaryContainer,
              child: ListTile(
                dense: true,
                title: Text(banner, maxLines: 1, overflow: TextOverflow.ellipsis),
                trailing: IconButton(
                  icon: const Icon(Icons.close),
                  tooltip: 'Cancel',
                  onPressed: () => setState(() {
                    if (_editing != null) _input.clear();
                    _editing = null;
                    _replyingTo = null;
                  }),
                ),
              ),
            ),
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 4, 4, 8),
            child: Row(crossAxisAlignment: CrossAxisAlignment.end, children: [
              Expanded(
                child: TextField(
                  controller: _input,
                  minLines: 1,
                  maxLines: 5,
                  textCapitalization: TextCapitalization.sentences,
                  onChanged: (v) {
                    // The core sends at most one of these every few seconds.
                    if (v.isNotEmpty) widget.state.engine.command({'cmd': 'typing', 'room_id': _room});
                  },
                  decoration: InputDecoration(
                    hintText: _thread != null ? 'Reply in thread' : 'Message ${room?.title ?? ''}',
                    border: OutlineInputBorder(borderRadius: BorderRadius.circular(24)),
                    contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
                  ),
                ),
              ),
              IconButton.filled(
                  onPressed: _send, tooltip: 'Send', icon: Icon(_editing != null ? Icons.check : Icons.send)),
            ]),
          ),
        ]),
      ),
    );
  }
}
