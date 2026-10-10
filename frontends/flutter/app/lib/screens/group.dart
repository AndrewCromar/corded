import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'chat.dart';
import 'common.dart';
import 'presence.dart';

/// Everyone on the current server except this person.
Future<List<Member>> otherMembers(AppState state) async {
  final r = await state.engine.command({'cmd': 'member_list', 'server_id': state.server!.id});
  final list = [
    for (final m in (r['members'] as List? ?? const [])) Member.fromJson((m as Map).cast<String, dynamic>())
  ].where((m) => !m.me).toList();
  list.sort((a, b) => a.displayName.toLowerCase().compareTo(b.displayName.toLowerCase()));
  return list;
}

/// Picks people and starts a group chat with them.
class NewGroupScreen extends StatefulWidget {
  const NewGroupScreen({super.key, required this.state});
  final AppState state;

  @override
  State<NewGroupScreen> createState() => _NewGroupScreenState();
}

class _NewGroupScreenState extends State<NewGroupScreen> {
  final _name = TextEditingController();
  final _picked = <String>{};
  List<Member>? _members;
  String? _error;

  @override
  void initState() {
    super.initState();
    otherMembers(widget.state).then((list) {
      if (mounted) setState(() => _members = list);
    }).catchError((Object e) {
      if (mounted) setState(() => _error = sentence(e is CordedError ? e.message : '$e'));
    });
  }

  Future<void> _create() async {
    final navigator = Navigator.of(context);
    await attempt(context, () async {
      final r = await widget.state.engine.command({
        'cmd': 'create_room',
        'server_id': widget.state.server!.id,
        'usernames': _picked.toList(),
        if (_name.text.trim().isNotEmpty) 'name': _name.text.trim(),
      });
      final room = (r['room'] as Map?)?['room_id'] as String?;
      if (room != null) {
        navigator.pushReplacement(
            MaterialPageRoute(builder: (_) => ChatScreen(state: widget.state, roomId: room)));
      } else {
        navigator.pop();
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final members = _members;
    final serverId = widget.state.server!.id;
    return Scaffold(
      appBar: AppBar(title: const Text('New group')),
      body: Column(children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
          child: TextField(
            controller: _name,
            textCapitalization: TextCapitalization.sentences,
            decoration:
                const InputDecoration(labelText: 'Group name (optional)', border: OutlineInputBorder()),
          ),
        ),
        Expanded(
          child: _error != null
              ? Center(child: Text(_error!))
              : members == null
                  ? const Center(child: CircularProgressIndicator())
                  : members.isEmpty
                      ? const Center(child: Text('Nobody else has joined this server yet.'))
                      : ListView(children: [
                          for (final m in members)
                            CheckboxListTile(
                              secondary: PresenceAvatar(
                                  name: m.displayName,
                                  picture: widget.state.store.picture(m.userId),
                                  status: widget.state.store.presence(serverId, m.userId)),
                              title: Text(m.displayName),
                              subtitle: Text('@${m.username}'),
                              value: _picked.contains(m.username),
                              onChanged: (on) => setState(
                                  () => on == true ? _picked.add(m.username) : _picked.remove(m.username)),
                            ),
                        ]),
        ),
        SafeArea(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: SizedBox(
              width: double.infinity,
              child: FilledButton(
                onPressed: _picked.isEmpty ? null : _create,
                child: Padding(
                  padding: const EdgeInsets.symmetric(vertical: 12),
                  child: Text(_picked.isEmpty
                      ? 'Pick at least one person'
                      : 'Create group (${_picked.length + 1} people)'),
                ),
              ),
            ),
          ),
        ),
      ]),
    );
  }
}

/// Asks which of the people not yet in a chat to add; null if none was picked.
Future<Member?> pickMember(BuildContext context, AppState state, Set<String> alreadyIn) async {
  final List<Member> members;
  try {
    members = (await otherMembers(state)).where((m) => !alreadyIn.contains(m.userId)).toList();
  } on CordedError {
    return null;
  }
  if (!context.mounted) return null;
  return showModalBottomSheet<Member>(
    context: context,
    showDragHandle: true,
    builder: (sheet) => SafeArea(
      child: ListView(shrinkWrap: true, children: [
        const ListTile(title: Text('Add to this group')),
        if (members.isEmpty) const ListTile(title: Text('Everyone on this server is already here.')),
        for (final m in members)
          ListTile(
            leading: CircleAvatar(child: Text(m.displayName.isEmpty ? '?' : m.displayName[0].toUpperCase())),
            title: Text(m.displayName),
            subtitle: Text('@${m.username}'),
            onTap: () => Navigator.pop(sheet, m),
          ),
      ]),
    ),
  );
}
