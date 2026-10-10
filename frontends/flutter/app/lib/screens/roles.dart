import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'common.dart';

/// What each permission means, in the order they are offered.
const permissionLabels = <String, (String, String)>{
  'view_channel': ('Read channels', 'See channels and read what is said in them'),
  'send_messages': ('Write messages', 'Send messages in channels'),
  'add_reactions': ('React', 'Add reactions to messages'),
  'attach_files': ('Send files', 'Attach files and pictures (when that exists)'),
  'mention_everyone': ('Mention everyone', 'Use @everyone'),
  'create_invite': ('Invite people', 'Make invite links'),
  'manage_messages': ('Manage messages', 'Delete and pin other people\'s messages'),
  'manage_nicknames': ('Manage display names', 'Change other people\'s display names'),
  'kick_members': ('Kick', 'Disconnect members'),
  'ban_members': ('Ban', 'Ban members and lift bans'),
  'manage_channels': ('Manage channels', 'Create, rename and delete channels'),
  'manage_roles': ('Manage roles', 'Create and edit roles, and give them to members'),
  'manage_server': ('Manage the server', 'Change server settings and restart it'),
  'administrator': ('Administrator', 'Everything above, everywhere'),
};

/// The roles of the current server; for those who may manage roles.
class RolesScreen extends StatelessWidget {
  const RolesScreen({super.key, required this.state});
  final AppState state;

  Future<void> _newRole(BuildContext context) async {
    final controller = TextEditingController();
    final name = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('New role'),
        content: TextField(
          controller: controller,
          autofocus: true,
          autocorrect: false,
          decoration: const InputDecoration(labelText: 'Name', helperText: 'For example: moderator'),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, controller.text.trim()), child: const Text('Create')),
        ],
      ),
    );
    if (name == null || name.isEmpty || !context.mounted) return;
    await attempt(
        context,
        () => state.engine.command(
            {'cmd': 'create_role', 'server_id': state.server!.id, 'name': name, 'permissions': <String>[]}));
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: state,
      builder: (context, _) {
        final roles = state.server?.roleDetails ?? const <Role>[];
        return Scaffold(
          appBar: AppBar(title: const Text('Roles')),
          body: ListView(children: [
            const Padding(
              padding: EdgeInsets.fromLTRB(16, 12, 16, 4),
              child: Text('A role is a set of permissions. Give roles to people on the Members screen. '
                  'What "everyone" may do applies to every member.'),
            ),
            for (final r in roles)
              ListTile(
                leading: Icon(r.isEveryone ? Icons.groups_outlined : Icons.shield_outlined),
                title: Text(r.isEveryone ? 'Everyone' : r.name),
                subtitle: Text(
                    r.permissions.isEmpty
                        ? 'No permissions'
                        : r.permissions.map((p) => permissionLabels[p]?.$1 ?? p).join(', '),
                    maxLines: 2,
                    overflow: TextOverflow.ellipsis),
                trailing: const Icon(Icons.chevron_right),
                onTap: () => Navigator.push(
                    context, MaterialPageRoute(builder: (_) => RoleScreen(state: state, roleName: r.name))),
              ),
            ListTile(
                leading: const Icon(Icons.add),
                title: const Text('New role'),
                onTap: () => _newRole(context)),
          ]),
        );
      },
    );
  }
}

/// One role: tick what it may do, or delete it.
class RoleScreen extends StatefulWidget {
  const RoleScreen({super.key, required this.state, required this.roleName});
  final AppState state;
  final String roleName;

  @override
  State<RoleScreen> createState() => _RoleScreenState();
}

class _RoleScreenState extends State<RoleScreen> {
  Set<String>? _edited; // null until something is changed

  Role? get _role => widget.state.server?.roleDetails.where((r) => r.name == widget.roleName).firstOrNull;

  Future<void> _save() async {
    final navigator = Navigator.of(context);
    final ok = await attempt(
        context,
        () => widget.state.engine.command({
              'cmd': 'edit_role',
              'server_id': widget.state.server!.id,
              'role': widget.roleName,
              'name': widget.roleName, // unchanged; only the permissions are edited here
              'permissions': _edited!.toList(),
            }));
    if (ok) navigator.pop();
  }

  Future<void> _delete() async {
    final navigator = Navigator.of(context);
    final go = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Delete the role ${widget.roleName}?'),
        content: const Text('Everyone who has it loses what it allowed.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Delete')),
        ],
      ),
    );
    if (go != true || !mounted) return;
    final ok = await attempt(
        context,
        () => widget.state.engine
            .command({'cmd': 'delete_role', 'server_id': widget.state.server!.id, 'role': widget.roleName}));
    if (ok) navigator.pop();
  }

  @override
  Widget build(BuildContext context) {
    final role = _role;
    if (role == null) {
      return Scaffold(appBar: AppBar(), body: const Center(child: Text('This role no longer exists.')));
    }
    final held = _edited ?? role.permissions;
    return Scaffold(
      appBar: AppBar(
        title: Text(role.isEveryone ? 'Everyone' : role.name),
        actions: [
          if (!role.isEveryone)
            IconButton(tooltip: 'Delete role', icon: const Icon(Icons.delete_outline), onPressed: _delete),
        ],
      ),
      body: ListView(children: [
        for (final p in permissionLabels.entries)
          CheckboxListTile(
            title: Text(p.value.$1),
            subtitle: Text(p.value.$2),
            value: held.contains(p.key),
            onChanged: (on) => setState(() {
              final next = {...held};
              on == true ? next.add(p.key) : next.remove(p.key);
              _edited = next;
            }),
          ),
        const SizedBox(height: 80),
      ]),
      floatingActionButton: _edited == null
          ? null
          : FloatingActionButton.extended(
              onPressed: _save, icon: const Icon(Icons.check), label: const Text('Save')),
    );
  }
}
