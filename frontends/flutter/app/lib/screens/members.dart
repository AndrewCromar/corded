import 'dart:async';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'chat.dart';
import 'common.dart';
import 'presence.dart';
import 'profile.dart';

/// Everyone on the server. Those with the permission can manage them here.
class MembersScreen extends StatefulWidget {
  const MembersScreen({super.key, required this.state});
  final AppState state;

  @override
  State<MembersScreen> createState() => _MembersScreenState();
}

class _MembersScreenState extends State<MembersScreen> {
  List<Member>? _members;
  String? _error;

  ServerInfo get _server => widget.state.server!;

  StreamSubscription<void>? _sub;

  @override
  void initState() {
    super.initState();
    _load();
    // Pictures are looked up as they are first asked for and arrive a moment
    // later; presence changes too. Either way the list is drawn again.
    _sub = widget.state.store.changes.listen((_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  Future<Map<String, dynamic>> _command(Map<String, dynamic> cmd) =>
      widget.state.engine.command({...cmd, 'server_id': _server.id});

  Future<void> _load() async {
    try {
      final r = await _command({'cmd': 'member_list'});
      final list = [
        for (final m in (r['members'] as List? ?? const []))
          Member.fromJson((m as Map).cast<String, dynamic>())
      ];
      int rank(Member m) => m.isOwner
          ? 0
          : m.isAdmin
              ? 1
              : 2;
      list.sort((a, b) {
        final byRank = rank(a).compareTo(rank(b));
        return byRank != 0 ? byRank : a.displayName.toLowerCase().compareTo(b.displayName.toLowerCase());
      });
      if (mounted) {
        setState(() {
          _members = list;
          _error = null;
        });
      }
    } on CordedError catch (e) {
      if (mounted) setState(() => _error = sentence(e.message));
    }
  }

  // Runs a management command, says what happened, and refreshes the list.
  Future<void> _do(Map<String, dynamic> cmd, String done) async {
    final messenger = ScaffoldMessenger.of(context);
    if (await attempt(context, () => _command(cmd))) {
      messenger.showSnackBar(SnackBar(content: Text(done)));
    }
    await _load();
  }

  Future<bool> _confirm(String title, String body, String action) async =>
      await showDialog<bool>(
        context: context,
        builder: (context) => AlertDialog(
          title: Text(title),
          content: Text(body),
          actions: [
            TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
            FilledButton(onPressed: () => Navigator.pop(context, true), child: Text(action)),
          ],
        ),
      ) ??
      false;

  Future<void> _rename(Member m) async {
    final controller = TextEditingController(text: m.nickname);
    final name = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(m.me ? 'Your display name' : 'Display name for ${m.username}'),
        content: TextField(
          controller: controller,
          autofocus: true,
          decoration: const InputDecoration(hintText: 'Leave empty to use the username'),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, controller.text.trim()), child: const Text('Save')),
        ],
      ),
    );
    if (name == null) return;
    await _do({'cmd': 'set_nickname', 'nickname': name, if (!m.me) 'username': m.username},
        name.isEmpty ? 'Display name cleared.' : 'Display name changed.');
  }

  Future<void> _roles(Member m) async {
    final held = m.roles.toSet();
    await showModalBottomSheet<void>(
      context: context,
      showDragHandle: true,
      builder: (sheet) => StatefulBuilder(
        builder: (context, setSheet) => SafeArea(
          child: ListView(shrinkWrap: true, children: [
            ListTile(title: Text('Roles for ${m.displayName}')),
            if (_server.roles.isEmpty)
              const ListTile(
                  title:
                      Text('This server has no roles yet. Roles are made in the terminal client for now.')),
            for (final role in _server.roles)
              SwitchListTile(
                title: Text(role),
                value: held.contains(role),
                onChanged: (on) async {
                  final ok = await attempt(
                      this.context,
                      () =>
                          _command({'cmd': 'grant_role', 'username': m.username, 'role': role, 'grant': on}));
                  if (ok) setSheet(() => on ? held.add(role) : held.remove(role));
                },
              ),
          ]),
        ),
      ),
    );
    await _load();
  }

  Future<void> _message(Member m) async {
    final navigator = Navigator.of(context);
    await attempt(context, () async {
      final r = await _command({'cmd': 'start_chat', 'username': m.username});
      final room = (r['room'] as Map?)?['room_id'] as String?;
      if (room != null) {
        navigator.push(MaterialPageRoute(builder: (_) => ChatScreen(state: widget.state, roomId: room)));
      }
    });
  }

  void _actions(Member m) {
    // Nobody manages the owner, and you do not kick yourself.
    final manageable = !m.me && !m.isOwner;
    showModalBottomSheet<void>(
      context: context,
      showDragHandle: true,
      builder: (sheet) {
        Widget item(IconData icon, String label, Future<void> Function() run, {bool danger = false}) =>
            ListTile(
              leading: Icon(icon, color: danger ? Theme.of(context).colorScheme.error : null),
              title: Text(label),
              onTap: () {
                Navigator.pop(sheet);
                run();
              },
            );
        return SafeArea(
          child: ListView(shrinkWrap: true, children: [
            ListTile(
              title: Text(m.displayName),
              subtitle: Text([
                '@${m.username}',
                if (m.isOwner) 'owner' else if (m.isAdmin) 'administrator',
                ...m.roles,
              ].join(' · ')),
            ),
            item(Icons.account_circle_outlined, 'View profile', () async {
              await Navigator.push(
                  context,
                  MaterialPageRoute(
                      builder: (_) =>
                          ProfileScreen(state: widget.state, userId: m.userId, username: m.username)));
            }),
            if (!m.me) item(Icons.chat_outlined, 'Message', () => _message(m)),
            if (m.me || _server.can('manage_nicknames'))
              item(Icons.badge_outlined, 'Change display name', () => _rename(m)),
            if (_server.can('manage_roles')) item(Icons.shield_outlined, 'Roles', () => _roles(m)),
            if (manageable && _server.can('kick_members'))
              item(Icons.logout, 'Kick', () async {
                if (await _confirm(
                    'Kick ${m.displayName}?',
                    'They are disconnected now. They can come straight back unless they are also banned.',
                    'Kick')) {
                  await _do({'cmd': 'kick', 'username': m.username}, '${m.displayName} was kicked.');
                }
              }),
            if (manageable && _server.can('ban_members'))
              item(Icons.block, 'Ban', () async {
                if (await _confirm('Ban ${m.displayName}?',
                    'They are disconnected and cannot sign in again until someone lifts the ban.', 'Ban')) {
                  await _do({'cmd': 'ban_user', 'username': m.username, 'banned': true},
                      '${m.displayName} was banned.');
                }
              }, danger: true),
            if (manageable && _server.can('ban_members'))
              item(Icons.lock_open, 'Lift ban',
                  () => _do({'cmd': 'ban_user', 'username': m.username, 'banned': false}, 'Ban lifted.')),
            if (manageable && _server.isOwner)
              item(Icons.person_remove_outlined, 'Remove account', () async {
                if (await _confirm(
                    'Remove ${m.displayName}?',
                    'Their account is deleted from this server and the username becomes free. '
                        'Messages they already sent stay on other people\'s devices. This cannot be undone.',
                    'Remove')) {
                  await _do({'cmd': 'remove_account', 'username': m.username}, '${m.username} was removed.');
                }
              }, danger: true),
          ]),
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    final members = _members;
    return Scaffold(
      appBar: AppBar(title: Text(members == null ? 'Members' : 'Members (${members.length})')),
      body: _error != null
          ? Center(
              child: Padding(
                  padding: const EdgeInsets.all(24), child: Text(_error!, textAlign: TextAlign.center)))
          : members == null
              ? const Center(child: CircularProgressIndicator())
              : RefreshIndicator(
                  onRefresh: _load,
                  child: ListView(children: [
                    for (final m in members)
                      ListTile(
                        leading: PresenceAvatar(
                            name: m.displayName,
                            picture: widget.state.store.picture(m.userId),
                            status: widget.state.store.presence(_server.id, m.userId)),
                        title: Row(mainAxisSize: MainAxisSize.min, children: [
                          Flexible(
                              child: Text(m.me ? '${m.displayName} (you)' : m.displayName,
                                  overflow: TextOverflow.ellipsis)),
                          if (m.bot) const BotTag(),
                        ]),
                        subtitle: Text([
                          presenceLook(widget.state.store.presence(_server.id, m.userId)).label,
                          '@${m.username}',
                          if (m.isOwner) 'owner' else if (m.isAdmin) 'administrator',
                          ...m.roles,
                        ].join(' · ')),
                        onTap: () => _actions(m),
                      ),
                  ]),
                ),
    );
  }
}
