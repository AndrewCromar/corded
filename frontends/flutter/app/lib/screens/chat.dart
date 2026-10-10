import 'dart:async';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_state.dart';
import 'common.dart';
import 'group.dart';
import 'linked_text.dart';
import 'swipe_to_reply.dart';

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

  static const _spans = {0: 'Off', 3600: '1 hour', 86400: '1 day', 604800: '1 week'};
  static String _spanName(int seconds) => _spans[seconds] ?? '${(seconds / 3600).round()} hours';

  // What the menu at the top of a chat does.
  Future<void> _chatAction(String choice, Room room) async {
    final navigator = Navigator.of(context);
    final engine = widget.state.engine;
    switch (choice) {
      case 'disappear':
        final seconds = await showDialog<int>(
          context: context,
          builder: (context) => SimpleDialog(
            title: const Text('Disappearing messages'),
            children: [
              const Padding(
                padding: EdgeInsets.fromLTRB(24, 0, 24, 8),
                child: Text('New messages in this chat erase themselves, for everyone, after:'),
              ),
              for (final s in _spans.entries)
                ListTile(
                  leading: Icon(s.key == room.disappearAfter
                      ? Icons.radio_button_checked
                      : Icons.radio_button_unchecked),
                  title: Text(s.value),
                  onTap: () => Navigator.pop(context, s.key),
                ),
            ],
          ),
        );
        if (seconds == null || seconds == room.disappearAfter || !mounted) return;
        await attempt(
            context, () => engine.command({'cmd': 'set_disappearing', 'room_id': _room, 'seconds': seconds}));
      case 'add':
        final member = await pickMember(context, widget.state, {for (final m in room.members) m.userId});
        if (member == null || !mounted) return;
        await attempt(context,
            () => engine.command({'cmd': 'add_member', 'room_id': _room, 'username': member.username}));
      case 'rename':
        final controller = TextEditingController(text: room.kind == 'group' ? room.title : '');
        final name = await showDialog<String>(
          context: context,
          builder: (context) => AlertDialog(
            title: const Text('Rename group'),
            content: TextField(controller: controller, autofocus: true),
            actions: [
              TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
              FilledButton(
                  onPressed: () => Navigator.pop(context, controller.text.trim()), child: const Text('Save')),
            ],
          ),
        );
        if (name == null || name.isEmpty || !mounted) return;
        await attempt(
            context, () => engine.command({'cmd': 'set_room_name', 'room_id': _room, 'name': name}));
      case 'close':
        final close = await showDialog<bool>(
          context: context,
          builder: (context) => AlertDialog(
            title: const Text('Close this chat?'),
            content: const Text('It leaves your list and its messages are removed from this device. '
                'The other person keeps their copy. Messaging them again starts a new chat.'),
            actions: [
              TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
              FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Close chat')),
            ],
          ),
        );
        if (close != true || !mounted) return;
        if (await attempt(context, () => engine.command({'cmd': 'leave_room', 'room_id': _room}))) {
          navigator.pop();
        }
      case 'leave':
        final leave = await showDialog<bool>(
          context: context,
          builder: (context) => AlertDialog(
            title: Text('Leave ${room.title}?'),
            content: const Text('You stop receiving its messages. Someone in the group can add you back.'),
            actions: [
              TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
              FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Leave')),
            ],
          ),
        );
        if (leave != true || !mounted) return;
        if (await attempt(context, () => engine.command({'cmd': 'leave_room', 'room_id': _room}))) {
          navigator.pop();
        }
    }
  }

  // While an "@" word is being typed at the end of the box, the people it could be.
  static final _typingMention = RegExp(r'(^|\s)@([A-Za-z0-9_-]*)$');

  List<String> _mentionChoices(Room? room) {
    if (room == null) return const [];
    final match = _typingMention.firstMatch(_input.text);
    if (match == null) return const [];
    final typed = match.group(2)!.toLowerCase();
    final names = [
      for (final m in room.members)
        if (!m.me) m.username,
      if (room.kind != 'direct') 'everyone',
    ];
    return [
      for (final n in names)
        if (n.startsWith(typed) && n != typed) n
    ].take(8).toList();
  }

  void _completeMention(String name) {
    final text = _input.text;
    final at = text.lastIndexOf('@');
    if (at < 0) return;
    final next = '${text.substring(0, at)}@$name ';
    _input.value = TextEditingValue(text: next, selection: TextSelection.collapsed(offset: next.length));
    setState(() {});
  }

  bool _isPinned(Message m) => _store.rooms[_room]?.pinned.contains(m.id) ?? false;

  // In a channel pinning is for those who manage messages; elsewhere anyone may.
  bool get _mayPin {
    final room = _store.rooms[_room];
    if (room == null) return false;
    return room.kind != 'channel' || (widget.state.server?.can('manage_messages') ?? false);
  }

  // The pinned messages, newest pin first; tap one to go to it.
  void _showPinned(Room room) {
    showModalBottomSheet<void>(
      context: context,
      showDragHandle: true,
      builder: (sheet) => SafeArea(
        child: ListView(shrinkWrap: true, children: [
          const ListTile(title: Text('Pinned messages')),
          for (final id in room.pinned.reversed)
            () {
              final m = _store.message(_room, id);
              return ListTile(
                leading: const Icon(Icons.push_pin_outlined),
                title: Text(m == null ? 'A message not loaded on this device yet' : m.text,
                    maxLines: 2, overflow: TextOverflow.ellipsis),
                subtitle: m == null ? null : Text(m.sender),
                trailing: _mayPin
                    ? IconButton(
                        tooltip: 'Unpin',
                        icon: const Icon(Icons.close),
                        onPressed: () {
                          Navigator.pop(sheet);
                          attempt(
                              context,
                              () => widget.state.engine.command(
                                  {'cmd': 'pin_event', 'room_id': _room, 'event_id': id, 'pinned': false}));
                        },
                      )
                    : null,
                onTap: () {
                  Navigator.pop(sheet);
                  _jumpTo(id);
                },
              );
            }(),
        ]),
      ),
    );
  }

  // "#name" in a message leads to that channel on this server.
  Map<String, String> get _channelLinks {
    final here = _store.rooms[_room]?.serverId;
    return {
      for (final r in _store.rooms.values)
        if (r.kind == 'channel' && r.serverId == here) r.title.replaceFirst('#', '').toLowerCase(): r.id
    };
  }

  void _openChannel(String roomId) {
    if (roomId == _room && _thread == null) return;
    Navigator.push(
        context, MaterialPageRoute(builder: (_) => ChatScreen(state: widget.state, roomId: roomId)));
  }

  final _keys = <String, GlobalKey>{};
  String? _highlight;

  // Scrolls to a message and lights it up. Only messages near the screen are
  // laid out, so an older one is reached by scrolling towards it, loading
  // earlier pages on the way if it is further back than what is loaded.
  Future<void> _jumpTo(String id) async {
    final messenger = ScaffoldMessenger.of(context);
    for (var step = 0; step < 60 && mounted; step++) {
      final target = _keys[id]?.currentContext;
      if (target != null && target.mounted) {
        await Scrollable.ensureVisible(target,
            alignment: 0.5, duration: const Duration(milliseconds: 250), curve: Curves.easeOut);
        if (!mounted) return;
        setState(() => _highlight = id);
        Timer(const Duration(milliseconds: 1600), () {
          if (mounted && _highlight == id) setState(() => _highlight = null);
        });
        return;
      }
      if (!_shown.any((m) => m.id == id)) {
        if (_thread != null || !_store.hasOlder(_room)) break;
        await _store.loadOlder(_room);
        continue;
      }
      if (!_scroll.hasClients) break;
      final position = _scroll.position;
      if (position.pixels >= position.maxScrollExtent) {
        await Future<void>.delayed(const Duration(milliseconds: 40));
        continue;
      }
      final next = position.pixels + position.viewportDimension * 0.9;
      await _scroll.animateTo(next > position.maxScrollExtent ? position.maxScrollExtent : next,
          duration: const Duration(milliseconds: 110), curve: Curves.linear);
    }
    messenger.showSnackBar(const SnackBar(content: Text('That message is not on this device.')));
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
        // A message has one relation. In a thread it belongs to the thread,
        // so what it replies to travels in its content instead.
        await widget.state.engine.command({
          'cmd': 'send_event',
          'room_id': _room,
          'type': 'm.text',
          'content': {'body': text, if (_thread != null && replyingTo != null) 'reply_to': replyingTo.id},
          if (_thread != null)
            'relation': {'kind': 'thread', 'target': _thread}
          else if (replyingTo != null)
            'relation': {'kind': 'reply', 'target': replyingTo.id},
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
          // A reply inside a thread can start a thread of its own.
          if (m.id != _thread)
            ListTile(
              leading: const Icon(Icons.forum_outlined),
              title: Text(_store.threadCount(_room, m) > 0 ? 'Open thread' : 'Start a thread'),
              onTap: () {
                Navigator.pop(sheet);
                _openThread(m);
              },
            ),

          ListTile(
            leading: const Icon(Icons.copy),
            title: const Text('Copy text'),
            onTap: () {
              Navigator.pop(sheet);
              Clipboard.setData(ClipboardData(text: m.body));
            },
          ),
          if (_mayPin)
            ListTile(
              leading: Icon(_isPinned(m) ? Icons.push_pin : Icons.push_pin_outlined),
              title: Text(_isPinned(m) ? 'Unpin' : 'Pin'),
              onTap: () {
                Navigator.pop(sheet);
                attempt(
                    context,
                    () => widget.state.engine.command(
                        {'cmd': 'pin_event', 'room_id': _room, 'event_id': m.id, 'pinned': !_isPinned(m)}));
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
    return sameDay
        ? hm
        : '${t.year}-${t.month.toString().padLeft(2, '0')}-${t.day.toString().padLeft(2, '0')} $hm';
  }

  Widget _bubble(Message m, List<Message> all) {
    final gone = m.status == 'redacted' || m.status == 'undecryptable';
    return SwipeToReply(
      enabled: !gone,
      onReply: () => setState(() {
        _replyingTo = m;
        _editing = null;
      }),
      child: _bubbleBody(m, all, gone),
    );
  }

  Widget _bubbleBody(Message m, List<Message> all, bool gone) {
    final theme = Theme.of(context);
    final scheme = theme.colorScheme;
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
              child: Container(
                margin: const EdgeInsets.fromLTRB(12, 4, 12, 0),
                padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                decoration: BoxDecoration(
                  // A message that mentions you stands out from the rest.
                  color: m.mine
                      ? scheme.primaryContainer
                      : m.mentionsMe
                          ? scheme.tertiaryContainer
                          : scheme.surfaceContainerHighest,
                  borderRadius: BorderRadius.circular(14),
                  // Lit up for a moment after jumping here; a message that
                  // mentions you keeps a coloured edge.
                  border: Border.all(
                      color: m.id == _highlight
                          ? scheme.primary
                          : m.mentionsMe
                              ? scheme.tertiary
                              : Colors.transparent,
                      width: 2),
                ),
                child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  if (m.mentionsMe)
                    Padding(
                      padding: const EdgeInsets.only(bottom: 2),
                      child: Row(mainAxisSize: MainAxisSize.min, children: [
                        Icon(Icons.alternate_email, size: 13, color: scheme.tertiary),
                        const SizedBox(width: 4),
                        Text('Mentioned you',
                            style: theme.textTheme.labelSmall
                                ?.copyWith(color: scheme.tertiary, fontWeight: FontWeight.bold)),
                      ]),
                    ),
                  if (_isPinned(m))
                    Padding(
                      padding: const EdgeInsets.only(bottom: 2),
                      child: Row(mainAxisSize: MainAxisSize.min, children: [
                        Icon(Icons.push_pin, size: 13, color: foreground.withValues(alpha: 0.7)),
                        const SizedBox(width: 4),
                        Text('Pinned',
                            style: theme.textTheme.labelSmall
                                ?.copyWith(color: foreground.withValues(alpha: 0.7))),
                      ]),
                    ),
                  if (!m.mine)
                    Text(m.sender,
                        style: theme.textTheme.labelMedium
                            ?.copyWith(color: scheme.primary, fontWeight: FontWeight.bold)),
                  if (m.replyTo != null)
                    // Tap the quoted line to go to the message it quotes.
                    GestureDetector(
                      behavior: HitTestBehavior.opaque,
                      onTap: () => _jumpTo(m.replyTo!),
                      child: Container(
                        margin: const EdgeInsets.only(top: 2, bottom: 4),
                        padding: const EdgeInsets.only(left: 8),
                        decoration:
                            BoxDecoration(border: Border(left: BorderSide(color: scheme.primary, width: 3))),
                        child: Text(
                          quoted == null ? 'A message not shown here' : '${quoted.sender}: ${quoted.text}',
                          maxLines: 2,
                          overflow: TextOverflow.ellipsis,
                          style:
                              theme.textTheme.bodySmall?.copyWith(color: foreground.withValues(alpha: 0.75)),
                        ),
                      ),
                    ),
                  LinkedText(gone ? m.text : m.body,
                      channels: _channelLinks,
                      onChannel: _openChannel,
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
            if (m.id != _thread && _store.threadCount(_room, m) > 0)
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
                            final last = replies.isEmpty
                                ? ''
                                : '  ·  last from ${replies.last.mine ? 'you' : replies.last.sender}';
                            return '${n == 1 ? 'Thread: 1 reply' : 'Thread: $n replies'}$last';
                          }(),
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style:
                                  theme.textTheme.labelLarge?.copyWith(color: scheme.onSecondaryContainer)),
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

  // An NSFW channel stays covered until the person chooses to open it, each
  // time they come to it.
  bool _uncovered = false;

  Widget _nsfwGate(Room room) {
    final theme = Theme.of(context);
    return Scaffold(
      appBar: AppBar(title: Text(room.title)),
      body: SafeArea(
        child: Center(
          child: Padding(
            padding: const EdgeInsets.all(32),
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              Icon(Icons.visibility_off_outlined, size: 56, color: theme.colorScheme.error),
              const SizedBox(height: 16),
              Text('NSFW channel', style: theme.textTheme.headlineSmall),
              const SizedBox(height: 8),
              Text(
                  '${room.title} is marked NSFW. It may contain content you do not want on your screen right now.',
                  textAlign: TextAlign.center),
              const SizedBox(height: 24),
              FilledButton(onPressed: () => Navigator.pop(context), child: const Text('Go back')),
              const SizedBox(height: 8),
              TextButton(
                  onPressed: () => setState(() => _uncovered = true), child: const Text('Open anyway')),
            ]),
          ),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final room = _store.rooms[_room];
    if (room != null && room.nsfw && !_uncovered) return _nsfwGate(room);
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
          Text(_thread != null ? 'Thread' : room?.title ?? 'Chat',
              maxLines: 1, overflow: TextOverflow.ellipsis),
          if (_thread != null)
            Text(room?.title ?? '', style: theme.textTheme.labelSmall)
          else if (room != null && room.kind != 'direct')
            Text('${room.members.length} members', style: theme.textTheme.labelSmall),
        ]),
        actions: [
          if (_thread == null)
            IconButton(
              tooltip: widget.state.isMuted(_room) ? 'Turn notifications back on' : 'Mute notifications',
              icon: Icon(
                  widget.state.isMuted(_room) ? Icons.notifications_off_outlined : Icons.notifications_none),
              onPressed: () async {
                final messenger = ScaffoldMessenger.of(context);
                final mute = !widget.state.isMuted(_room);
                await widget.state.setMuted(_room, mute);
                if (mounted) setState(() {});
                messenger.showSnackBar(SnackBar(
                    content: Text(mute
                        ? 'Muted. No notifications from this chat on this phone.'
                        : 'Notifications are back on for this chat.')));
              },
            ),
          if (_thread == null && room != null)
            PopupMenuButton<String>(
              tooltip: 'More',
              onSelected: (choice) => _chatAction(choice, room),
              itemBuilder: (context) => [
                PopupMenuItem(
                    value: 'disappear',
                    child: Text(room.disappearAfter > 0
                        ? 'Disappearing messages: ${_spanName(room.disappearAfter)}'
                        : 'Disappearing messages')),
                if (room.kind == 'direct')
                  const PopupMenuItem(value: 'close', child: Text('Close this chat')),
                if (room.kind == 'group') ...const [
                  PopupMenuItem(value: 'add', child: Text('Add people')),
                  PopupMenuItem(value: 'rename', child: Text('Rename group')),
                  PopupMenuItem(value: 'leave', child: Text('Leave group')),
                ],
              ],
            ),
        ],
      ),
      body: SafeArea(
        child: Column(children: [
          if (_thread == null && room != null && room.pinned.isNotEmpty)
            Material(
              color: theme.colorScheme.surfaceContainerHigh,
              child: InkWell(
                onTap: () => _showPinned(room),
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                  child: Row(children: [
                    Icon(Icons.push_pin, size: 16, color: theme.colorScheme.primary),
                    const SizedBox(width: 8),
                    Expanded(
                      child: Text(() {
                        final latest = _store.message(_room, room.pinned.last);
                        final count = room.pinned.length == 1 ? 'Pinned' : '${room.pinned.length} pinned';
                        return latest == null ? count : '$count: ${latest.text}';
                      }(), maxLines: 1, overflow: TextOverflow.ellipsis, style: theme.textTheme.labelLarge),
                    ),
                    const Icon(Icons.expand_more, size: 18),
                  ]),
                ),
              ),
            ),
          Expanded(
            child: messages.isEmpty
                ? Center(
                    child: Text(_thread != null ? 'This thread is empty.' : 'No messages yet. Say hello.'))
                : ListView.builder(
                    controller: _scroll,
                    reverse: true,
                    padding: const EdgeInsets.symmetric(vertical: 8),
                    itemCount: messages.length,
                    itemBuilder: (context, i) {
                      final m = messages[messages.length - 1 - i];
                      return KeyedSubtree(
                          key: _keys.putIfAbsent(m.id, GlobalKey.new), child: _bubble(m, messages));
                    },
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
          if (_mentionChoices(room).isNotEmpty)
            SizedBox(
              height: 44,
              child: ListView(
                scrollDirection: Axis.horizontal,
                padding: const EdgeInsets.symmetric(horizontal: 12),
                children: [
                  for (final name in _mentionChoices(room))
                    Padding(
                      padding: const EdgeInsets.only(right: 6),
                      child: ActionChip(label: Text('@$name'), onPressed: () => _completeMention(name)),
                    ),
                ],
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
                    setState(() {}); // the name suggestions follow what is typed
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
