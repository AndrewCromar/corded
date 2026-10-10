import 'dart:convert';
import 'dart:typed_data';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'add_server.dart';
import 'catch_up.dart';
import 'chat.dart';
import 'common.dart';
import 'group.dart';
import 'members.dart';
import 'presence.dart';
import 'search.dart';
import 'settings.dart';

/// The chats of one server, with a drawer for moving between servers.
class HomeScreen extends StatelessWidget {
  const HomeScreen({super.key, required this.state});
  final AppState state;

  static const _connectionWords = {
    'live': null, // nothing to say when all is well
    'connecting': 'Connecting...',
    'authenticating': 'Connecting...',
    'syncing': 'Catching up...',
    'backoff': 'Not connected. Trying again shortly.',
    'disconnected': 'Not connected',
  };

  Future<void> _startChat(BuildContext context) async {
    final controller = TextEditingController();
    final name = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Message someone'),
        content: TextField(
          controller: controller,
          autofocus: true,
          autocorrect: false,
          decoration: const InputDecoration(labelText: 'Their username'),
          onSubmitted: (v) => Navigator.pop(context, v),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, controller.text), child: const Text('Open')),
        ],
      ),
    );
    if (name == null || name.trim().isEmpty || !context.mounted) return;
    await attempt(context, () async {
      final r = await state.engine.command({
        'cmd': 'start_chat',
        'username': name.trim().toLowerCase(),
        if (state.server != null) 'server_id': state.server!.id,
      });
      final room = (r['room'] as Map?)?['room_id'] as String?;
      if (room != null && context.mounted) _open(context, room);
    });
  }

  // The app decides where a chat opens: over this list on a phone, beside it
  // in a wide desktop window.
  void _open(BuildContext context, String roomId) {
    final open = state.onOpenChat;
    if (open != null) {
      open(roomId, null);
    } else {
      Navigator.push(context, MaterialPageRoute(builder: (_) => ChatScreen(state: state, roomId: roomId)));
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final server = state.server;
    final rooms = server == null ? const <Room>[] : state.store.roomsOf(server.id);
    final status = _connectionWords.containsKey(server?.connection)
        ? _connectionWords[server?.connection]
        : sentence(server?.connection ?? '');
    // Channels the people who run the server pinned to the top, for everyone.
    final pinned = rooms.where((r) => r.kind == 'channel' && !r.archived && !r.nsfw && r.featured).toList();
    final channels =
        rooms.where((r) => r.kind == 'channel' && !r.archived && !r.nsfw && !r.featured).toList();
    // Kept apart from the everyday channels, further down the list.
    final nsfw = rooms.where((r) => r.kind == 'channel' && !r.archived && r.nsfw).toList();
    // In no section, or in one this device has not been told about.
    final known = {for (final s in server?.sections ?? const <Section>[]) s.id};
    final loose = channels.where((r) => !known.contains(r.section)).toList();
    final archived = rooms.where((r) => r.kind == 'channel' && r.archived).toList();
    final others = rooms.where((r) => r.kind != 'channel').toList();
    final unreadChats = rooms.where((r) => r.unread > 0).length;
    final unreadTotal = rooms.fold<int>(0, (sum, r) => sum + r.unread);

    // In a direct chat, how present the other person is.
    String? statusIn(Room r) {
      if (r.kind != 'direct') return null;
      final other = r.members.where((m) => !m.me).firstOrNull;
      return other == null ? null : state.store.presence(r.serverId, other.userId);
    }

    // What can be done to a chat without opening it: hold it, or right-click.
    void actions(Room r) => showModalBottomSheet<void>(
          context: context,
          showDragHandle: true,
          builder: (sheet) => SafeArea(
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              ListTile(
                leading: const Icon(Icons.mark_chat_unread_outlined),
                title: const Text('Mark as unread'),
                onTap: () {
                  Navigator.pop(sheet);
                  attempt(context, () => state.engine.command({'cmd': 'mark_unread', 'room_id': r.id}));
                },
              ),
              ListTile(
                leading: Icon(state.isMuted(r.id)
                    ? Icons.notifications_active_outlined
                    : Icons.notifications_off_outlined),
                title: Text(state.isMuted(r.id) ? 'Turn notifications back on' : 'Mute notifications'),
                onTap: () {
                  Navigator.pop(sheet);
                  state.setMuted(r.id, !state.isMuted(r.id));
                },
              ),
            ]),
          ),
        );
    Widget plainTile(Room r) => ListTile(
          leading: r.kind == 'direct'
              ? PresenceAvatar(
                  name: r.title,
                  radius: 16,
                  picture: () {
                    final other = r.members.where((m) => !m.me).firstOrNull;
                    return other == null ? null : state.store.picture(other.userId);
                  }(),
                  status: statusIn(r) ?? 'offline')
              : Icon(r.isTaskList
                  ? Icons.checklist
                  : r.kind == 'channel'
                      ? Icons.tag
                      : Icons.group_outlined),
          title: Text(r.kind == 'channel' ? r.title.replaceFirst('#', '') : r.title,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: r.unread > 0 ? const TextStyle(fontWeight: FontWeight.bold) : null),
          trailing: Row(mainAxisSize: MainAxisSize.min, children: [
            if (state.isMuted(r.id))
              Padding(
                padding: const EdgeInsets.only(right: 6),
                child: Icon(Icons.notifications_off_outlined, size: 18, color: theme.colorScheme.outline),
              ),
            if (r.nsfw)
              Padding(
                padding: const EdgeInsets.only(right: 6),
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                  decoration: BoxDecoration(
                      border: Border.all(color: theme.colorScheme.error),
                      borderRadius: BorderRadius.circular(6)),
                  child: Text('NSFW',
                      style: theme.textTheme.labelSmall
                          ?.copyWith(color: theme.colorScheme.error, fontWeight: FontWeight.bold)),
                ),
              ),
            if (r.unread > 0) Badge(label: Text('${r.unread}')),
          ]),
          selected: r.id == state.viewingRoom,
          onTap: () => _open(context, r.id),
          // Hold a chat for what can be done without opening it.
          onLongPress: () => actions(r),
        );
    Widget tile(Room r) => GestureDetector(onSecondaryTap: () => actions(r), child: plainTile(r));
    Widget heading(String text) => Padding(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 4),
          child: Text(text, style: theme.textTheme.labelLarge?.copyWith(color: theme.colorScheme.primary)),
        );

    return Scaffold(
      appBar: AppBar(
        title: server == null || server.description.isEmpty
            ? Text(server == null || server.name.isEmpty ? 'Corded' : server.name)
            : Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
                Text(server.name.isEmpty ? 'Corded' : server.name),
                Text(server.description,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: Theme.of(context).textTheme.labelSmall),
              ]),
        actions: [
          IconButton(
            tooltip: 'Search messages',
            icon: const Icon(Icons.search),
            onPressed: () =>
                Navigator.push(context, MaterialPageRoute(builder: (_) => SearchScreen(state: state))),
          ),
          if (server != null)
            IconButton(
              tooltip: 'Members',
              icon: const Icon(Icons.people_outline),
              onPressed: () =>
                  Navigator.push(context, MaterialPageRoute(builder: (_) => MembersScreen(state: state))),
            ),
        ],
      ),
      drawer: Drawer(
        child: SafeArea(
          child: ListView(children: [
            heading('Servers'),
            for (final s in state.store.servers.values)
              ListTile(
                leading: ServerIcon(s),
                title: Text(s.name.isEmpty ? s.address : s.name),
                subtitle: Text(s.description.isEmpty ? s.address : s.description,
                    maxLines: 2, overflow: TextOverflow.ellipsis),
                selected: s.id == server?.id,
                onTap: () {
                  state.selectServer(s.id);
                  Navigator.pop(context);
                },
              ),
            const Divider(),
            ListTile(
              leading: const Icon(Icons.add),
              title: const Text('Join another server'),
              onTap: () {
                Navigator.pop(context);
                Navigator.push(context, MaterialPageRoute(builder: (_) => AddServerScreen(state: state)));
              },
            ),
            // Opened seldom, so it lives here and not above the chats.
            ListTile(
              leading: const Icon(Icons.settings_outlined),
              title: const Text('Settings'),
              onTap: () {
                Navigator.pop(context);
                Navigator.push(context, MaterialPageRoute(builder: (_) => SettingsScreen(state: state)));
              },
            ),
          ]),
        ),
      ),
      body: Column(children: [
        if (status != null)
          Material(
            color: theme.colorScheme.secondaryContainer,
            child: ListTile(dense: true, leading: const Icon(Icons.cloud_off_outlined), title: Text(status)),
          ),
        Expanded(
          child: ListView(children: [
            // Everything unread, gathered in one place to go through.
            if (unreadTotal > 0)
              ListTile(
                leading: const Icon(Icons.mark_chat_unread_outlined),
                title: const Text('Catch up', style: TextStyle(fontWeight: FontWeight.bold)),
                subtitle: Text('$unreadChats chat${unreadChats == 1 ? '' : 's'} with something new'),
                trailing: Badge(label: Text('$unreadTotal')),
                onTap: () =>
                    Navigator.push(context, MaterialPageRoute(builder: (_) => CatchUpScreen(state: state))),
              ),
            if (pinned.isNotEmpty) heading('Pinned'),
            ...pinned.map(tile),
            // The server's own sections, in its order; then the channels in none.
            for (final section in server?.sections ?? const <Section>[])
              if (channels.any((r) => r.section == section.id)) ...[
                heading(section.name),
                ...channels.where((r) => r.section == section.id).map(tile),
              ],
            if (loose.isNotEmpty) heading('Channels'),
            ...loose.map(tile),
            heading('Direct messages'),
            ...others.map(tile),
            if (others.isEmpty)
              const ListTile(dense: true, title: Text('None yet. Use the button below to message someone.')),
            // Out of sight until asked for.
            if (nsfw.isNotEmpty)
              ExpansionTile(
                key: const PageStorageKey('hidden-channels'),
                title: Text('Hidden channels',
                    style: theme.textTheme.titleSmall?.copyWith(color: theme.colorScheme.primary)),
                tilePadding: const EdgeInsets.symmetric(horizontal: 16),
                shape: const Border(),
                collapsedShape: const Border(),
                children: [for (final r in nsfw) tile(r)],
              ),
            if (archived.isNotEmpty) heading('Archived'),
            for (final r in archived) Opacity(opacity: 0.55, child: tile(r)),
            const SizedBox(height: 80),
          ]),
        ),
      ]),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: () => showModalBottomSheet<void>(
          context: context,
          showDragHandle: true,
          builder: (sheet) => SafeArea(
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              ListTile(
                leading: const Icon(Icons.person_outline),
                title: const Text('Message someone'),
                onTap: () {
                  Navigator.pop(sheet);
                  _startChat(context);
                },
              ),
              ListTile(
                leading: const Icon(Icons.group_add_outlined),
                title: const Text('New group'),
                onTap: () {
                  Navigator.pop(sheet);
                  Navigator.push(context, MaterialPageRoute(builder: (_) => NewGroupScreen(state: state)));
                },
              ),
            ]),
          ),
        ),
        icon: const Icon(Icons.edit_outlined),
        label: const Text('Message'),
      ),
    );
  }
}

/// A server's picture, or a plain mark when it has none.
class ServerIcon extends StatelessWidget {
  const ServerIcon(this.server, {super.key, this.radius = 16});
  final ServerInfo server;
  final double radius;

  static final _decoded = <String, Uint8List?>{};

  @override
  Widget build(BuildContext context) {
    final bytes = server.icon.isEmpty
        ? null
        : _decoded.putIfAbsent(server.icon, () {
            try {
              return base64Decode(server.icon);
            } on FormatException {
              return null;
            }
          });
    if (bytes == null) return const Icon(Icons.dns_outlined);
    return CircleAvatar(radius: radius, backgroundImage: MemoryImage(bytes));
  }
}
