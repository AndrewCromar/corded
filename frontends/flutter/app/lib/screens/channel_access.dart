import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'common.dart';

/// What a role may do in one channel, in the four ways a person thinks of it.
enum ChannelAccess { same, none, read, write }

/// The access an exception amounts to. "same" means the channel makes no
/// exception for the role: it may do there what it may do everywhere.
ChannelAccess accessFrom(Set<String> allow, Set<String> deny) {
  if (deny.contains('view_channel')) return ChannelAccess.none;
  if (deny.contains('send_messages')) return ChannelAccess.read;
  if (allow.contains('view_channel') || allow.contains('send_messages')) return ChannelAccess.write;
  return ChannelAccess.same;
}

/// The exception that gives a role that access.
({List<String> allow, List<String> deny}) exceptionFor(ChannelAccess access) => switch (access) {
      ChannelAccess.same => (allow: const [], deny: const []),
      ChannelAccess.none => (allow: const [], deny: const ['view_channel', 'send_messages']),
      ChannelAccess.read => (allow: const ['view_channel'], deny: const ['send_messages']),
      ChannelAccess.write => (allow: const ['view_channel', 'send_messages'], deny: const []),
    };

const _labels = {
  ChannelAccess.same: ('Same as elsewhere', 'No special rule for this channel'),
  ChannelAccess.none: ('No access', 'Cannot see the channel'),
  ChannelAccess.read: ('Read', 'Can read, cannot write'),
  ChannelAccess.write: ('Read and write', 'Can read and write here'),
};

/// Who may read and write in one channel, role by role.
class ChannelAccessScreen extends StatefulWidget {
  const ChannelAccessScreen({super.key, required this.state, required this.room});
  final AppState state;
  final Room room;

  @override
  State<ChannelAccessScreen> createState() => _ChannelAccessScreenState();
}

class _ChannelAccessScreenState extends State<ChannelAccessScreen> {
  Map<String, ChannelAccess>? _access; // by role name; null while loading
  String? _error;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    try {
      final r = await widget.state.engine.command({'cmd': 'channel_access', 'room_id': widget.room.id});
      final found = <String, ChannelAccess>{};
      for (final e in (r['access'] as List? ?? const [])) {
        final entry = (e as Map).cast<String, dynamic>();
        found['${entry['role']}'] = accessFrom(
            ((entry['allow'] as List?) ?? const []).map((x) => '$x').toSet(),
            ((entry['deny'] as List?) ?? const []).map((x) => '$x').toSet());
      }
      if (mounted) setState(() => _access = found);
    } on CordedError catch (e) {
      if (mounted) setState(() => _error = sentence(e.message));
    }
  }

  Future<void> _choose(Role role) async {
    final current = _access?[role.name] ?? ChannelAccess.same;
    final picked = await showDialog<ChannelAccess>(
      context: context,
      builder: (context) => SimpleDialog(
        title: Text('${role.isEveryone ? 'Everyone' : role.name} in ${widget.room.title}'),
        children: [
          for (final option in ChannelAccess.values)
            ListTile(
              leading: Icon(option == current ? Icons.radio_button_checked : Icons.radio_button_unchecked),
              title: Text(_labels[option]!.$1),
              subtitle: Text(_labels[option]!.$2),
              onTap: () => Navigator.pop(context, option),
            ),
        ],
      ),
    );
    if (picked == null || picked == current || !mounted) return;
    final rule = exceptionFor(picked);
    final ok = await attempt(
        context,
        () => widget.state.engine.command({
              'cmd': 'set_channel_access',
              'room_id': widget.room.id,
              'role': role.name,
              'allow': rule.allow,
              'deny': rule.deny,
            }));
    if (ok && mounted) setState(() => _access![role.name] = picked);
  }

  @override
  Widget build(BuildContext context) {
    final roles = widget.state.server?.roleDetails ?? const <Role>[];
    final access = _access;
    return Scaffold(
      appBar: AppBar(title: Text('Access to ${widget.room.title}')),
      body: _error != null
          ? Center(
              child: Padding(
                  padding: const EdgeInsets.all(24), child: Text(_error!, textAlign: TextAlign.center)))
          : access == null
              ? const Center(child: CircularProgressIndicator())
              : ListView(children: [
                  const Padding(
                    padding: EdgeInsets.fromLTRB(16, 12, 16, 8),
                    child: Text(
                        'Choose what each role may do in this channel. A person with several roles gets '
                        'the most any of them allows. To make a channel private, give Everyone "No access" and '
                        'the roles that belong "Read and write". The owner and administrators always have access.'),
                  ),
                  for (final role in roles)
                    ListTile(
                      leading: Icon(role.isEveryone ? Icons.groups_outlined : Icons.shield_outlined),
                      title: Text(role.isEveryone ? 'Everyone' : role.name),
                      subtitle: Text(_labels[access[role.name] ?? ChannelAccess.same]!.$1),
                      trailing: const Icon(Icons.expand_more),
                      onTap: () => _choose(role),
                    ),
                ]),
    );
  }
}
