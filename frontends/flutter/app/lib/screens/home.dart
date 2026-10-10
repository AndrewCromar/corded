import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'add_server.dart';
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

  void _open(BuildContext context, String roomId) {
    Navigator.push(context, MaterialPageRoute(builder: (_) => ChatScreen(state: state, roomId: roomId)));
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final server = state.server;
    final rooms = server == null ? const <Room>[] : state.store.roomsOf(server.id);
    final status = _connectionWords.containsKey(server?.connection)
        ? _connectionWords[server?.connection]
        : sentence(server?.connection ?? '');
    final channels = rooms.where((r) => r.kind == 'channel').toList();
    final others = rooms.where((r) => r.kind != 'channel').toList();

    // In a direct chat, how present the other person is.
    String? statusIn(Room r) {
      if (r.kind != 'direct') return null;
      final other = r.members.where((m) => !m.me).firstOrNull;
      return other == null ? null : state.store.presence(r.serverId, other.userId);
    }

    Widget tile(Room r) => ListTile(
          leading: r.kind == 'direct'
              ? PresenceAvatar(
                  name: r.title,
                  radius: 16,
                  picture: () {
                    final other = r.members.where((m) => !m.me).firstOrNull;
                    return other == null ? null : state.store.picture(other.userId);
                  }(),
                  status: statusIn(r) ?? 'offline')
              : Icon(r.kind == 'channel' ? Icons.tag : Icons.group_outlined),
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
          onTap: () => _open(context, r.id),
        );
    Widget heading(String text) => Padding(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 4),
          child: Text(text, style: theme.textTheme.labelLarge?.copyWith(color: theme.colorScheme.primary)),
        );

    return Scaffold(
      appBar: AppBar(
        title: Text(server == null || server.name.isEmpty ? 'Corded' : server.name),
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
          IconButton(
            tooltip: 'Settings',
            icon: const Icon(Icons.settings_outlined),
            onPressed: () =>
                Navigator.push(context, MaterialPageRoute(builder: (_) => SettingsScreen(state: state))),
          ),
        ],
      ),
      drawer: Drawer(
        child: SafeArea(
          child: ListView(children: [
            heading('Servers'),
            for (final s in state.store.servers.values)
              ListTile(
                leading: const Icon(Icons.dns_outlined),
                title: Text(s.name.isEmpty ? s.address : s.name),
                subtitle: Text(s.address),
                selected: s.id == server?.id,
                onTap: () {
                  state.selectServer(s.id);
                  Navigator.pop(context);
                },
                trailing: IconButton(
                  tooltip: 'Leave this server',
                  icon: const Icon(Icons.logout),
                  onPressed: () async {
                    final leave = await showDialog<bool>(
                      context: context,
                      builder: (context) => AlertDialog(
                        title: Text('Leave ${s.name.isEmpty ? s.address : s.name}?'),
                        content: const Text('The server and its chats are removed from this device. '
                            'Your account stays on the server, so you can join again later as the same person.'),
                        actions: [
                          TextButton(
                              onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
                          FilledButton(
                              onPressed: () => Navigator.pop(context, true), child: const Text('Leave')),
                        ],
                      ),
                    );
                    if (leave != true || !context.mounted) return;
                    Navigator.pop(context);
                    await attempt(
                        context, () => state.engine.command({'cmd': 'forget_server', 'server_id': s.id}));
                  },
                ),
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
            if (channels.isNotEmpty) heading('Channels'),
            ...channels.map(tile),
            heading('Direct messages'),
            ...others.map(tile),
            if (others.isEmpty)
              const ListTile(dense: true, title: Text('None yet. Use the button below to message someone.')),
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
