import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:image_picker/image_picker.dart';

import '../app_state.dart';
import '../picture.dart';
import 'channel_access.dart';
import 'common.dart';
import 'home.dart';
import 'roles.dart';
import 'scan.dart';

/// Running the server from the app: channels, invites, settings, restart.
/// Each part shows only to those the server lets do it.
class ServerScreen extends StatefulWidget {
  const ServerScreen({super.key, required this.state});
  final AppState state;

  @override
  State<ServerScreen> createState() => _ServerScreenState();
}

class _ServerScreenState extends State<ServerScreen> {
  List<Map<String, dynamic>> _settings = const [];
  Map<String, dynamic> _status = const {};

  ServerInfo get _server => widget.state.server!;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<Map<String, dynamic>> _command(Map<String, dynamic> cmd) =>
      widget.state.engine.command({...cmd, 'server_id': _server.id});

  Future<void> _load() async {
    if (!_server.can('manage_server')) return;
    try {
      final s = await _command({'cmd': 'get_settings'});
      final st = await _command({'cmd': 'server_status'});
      if (!mounted) return;
      setState(() {
        _settings = [
          for (final e in (s['settings'] as List? ?? const [])) (e as Map).cast<String, dynamic>()
        ];
        _status = ((st['status'] as Map?) ?? const {}).cast<String, dynamic>();
      });
    } on CordedError {
      // Shown without them.
    }
  }

  Future<void> _do(Map<String, dynamic> cmd, String done) async {
    final messenger = ScaffoldMessenger.of(context);
    if (await attempt(context, () => _command(cmd))) {
      messenger.showSnackBar(SnackBar(content: Text(done)));
    }
    await _load();
    if (mounted) setState(() {});
  }

  Future<String?> _ask(String title, {String initial = '', String? hint, String action = 'Save'}) {
    final controller = TextEditingController(text: initial);
    return showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(title),
        content: TextField(
          controller: controller,
          autofocus: true,
          autocorrect: false,
          decoration: InputDecoration(helperText: hint, helperMaxLines: 4),
          onSubmitted: (v) => Navigator.pop(context, v.trim()),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, controller.text.trim()), child: Text(action)),
        ],
      ),
    );
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

  Future<void> _newChannel() async {
    final name = await _ask('New channel',
        hint: 'Spaces become dashes and capitals become lower case', action: 'Next');
    if (name == null || name.isEmpty || !mounted) return;
    // What kind: messages, or a list of tasks to tick off.
    final type = await showDialog<String>(
      context: context,
      builder: (context) => SimpleDialog(title: const Text('What kind of channel?'), children: [
        ListTile(
          leading: const Icon(Icons.tag),
          title: const Text('Messages'),
          subtitle: const Text('An ordinary channel'),
          onTap: () => Navigator.pop(context, ''),
        ),
        ListTile(
          leading: const Icon(Icons.checklist),
          title: const Text('Task list'),
          subtitle: const Text('Rows to tick off; anyone who can write can add and tick'),
          onTap: () => Navigator.pop(context, 'tasks'),
        ),
      ]),
    );
    if (type == null) return;
    await _do({'cmd': 'create_channel', 'name': tidyChannelName(name), if (type.isNotEmpty) 'type': type},
        type == 'tasks' ? 'Task list created.' : 'Channel created.');
  }

  void _channelActions(Room r) {
    final name = r.title.replaceFirst('#', '');
    showModalBottomSheet<void>(
      context: context,
      showDragHandle: true,
      builder: (sheet) => SafeArea(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          ListTile(title: Text(r.title)),
          ListTile(
            leading: const Icon(Icons.lock_outline),
            title: const Text('Who can read and write'),
            onTap: () {
              Navigator.pop(sheet);
              Navigator.push(context,
                  MaterialPageRoute(builder: (_) => ChannelAccessScreen(state: widget.state, room: r)));
            },
          ),
          ListTile(
            leading: Icon(r.archived ? Icons.unarchive_outlined : Icons.inventory_2_outlined),
            title: Text(r.archived ? 'Bring back from the archive' : 'Archive'),
            subtitle: const Text('Stays readable; nobody can write in it'),
            onTap: () {
              Navigator.pop(sheet);
              _do({'cmd': 'set_channel_archived', 'room_id': r.id, 'archived': !r.archived},
                  r.archived ? 'The channel is open again.' : 'Archived.');
            },
          ),
          ListTile(
            leading: Icon(r.nsfw ? Icons.visibility_outlined : Icons.visibility_off_outlined),
            title: Text(r.nsfw ? 'Remove the NSFW mark' : 'Mark as NSFW'),
            subtitle: const Text('People are warned before the channel opens'),
            onTap: () {
              Navigator.pop(sheet);
              _do({'cmd': 'set_channel_nsfw', 'room_id': r.id, 'nsfw': !r.nsfw},
                  r.nsfw ? 'The NSFW mark was removed.' : 'Marked as NSFW.');
            },
          ),
          ListTile(
            leading: const Icon(Icons.edit_outlined),
            title: const Text('Rename'),
            onTap: () async {
              Navigator.pop(sheet);
              final to = await _ask('Rename $name', initial: name);
              if (to == null || to.isEmpty || to == name) return;
              await _do({'cmd': 'rename_channel', 'room_id': r.id, 'name': tidyChannelName(to)},
                  'Channel renamed.');
            },
          ),
          ListTile(
            leading: Icon(Icons.delete_outline, color: Theme.of(context).colorScheme.error),
            title: const Text('Delete'),
            onTap: () async {
              Navigator.pop(sheet);
              if (await _confirm('Delete ${r.title}?',
                  'The channel and its messages disappear for everyone. This cannot be undone.', 'Delete')) {
                await _do({'cmd': 'delete_channel', 'room_id': r.id}, 'Channel deleted.');
              }
            },
          ),
        ]),
      ),
    );
  }

  Future<void> _invite() async {
    Map<String, dynamic>? invite;
    if (!await attempt(
        context, () async => invite = await _command({'cmd': 'create_invite', 'expires_in_hours': 168}))) {
      return;
    }
    if (!mounted) return;
    final link = '${invite!['link']}';
    await showDialog<void>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Invite link'),
        content:
            Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
          Center(child: CodeToScan(link)),
          const SizedBox(height: 12),
          SelectableText(link, style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
          const SizedBox(height: 12),
          const Text('Anyone with this link can join for the next 7 days. '
              'It carries the server\'s key, so they cannot be sent to an impostor.'),
        ]),
        actions: [
          TextButton(
              onPressed: () => Clipboard.setData(ClipboardData(text: link)), child: const Text('Copy')),
          FilledButton(onPressed: () => Navigator.pop(context), child: const Text('Done')),
        ],
      ),
    );
  }

  // Settings that take one of a few values are picked from a list, not typed.
  static const _choices = {
    'scope': {
      'machine': 'This computer only',
      'network': 'The local network',
      'internet': 'Anyone on the internet',
    },
    'registration': {
      'open': 'Anyone can create an account',
      'invite': 'Only with an invite',
      'closed': 'Nobody new',
    },
    'history_sharing': {'on': 'On', 'off': 'Off'},
    'housekeeping': {'on': 'On', 'off': 'Off'},
  };

  Future<String?> _choose(String key, String current, Map<String, String> options, String description) =>
      showDialog<String>(
        context: context,
        builder: (context) => SimpleDialog(
          title: Text(key),
          children: [
            Padding(padding: const EdgeInsets.fromLTRB(24, 0, 24, 8), child: Text(description)),
            for (final o in options.entries)
              ListTile(
                leading: Icon(o.key == current ? Icons.radio_button_checked : Icons.radio_button_unchecked),
                title: Text(o.value),
                subtitle: Text(o.key),
                onTap: () => Navigator.pop(context, o.key),
              ),
          ],
        ),
      );

  Future<void> _editSetting(Map<String, dynamic> s) async {
    final key = '${s['key']}';
    final options = _choices[key];
    final value = options != null
        ? await _choose(key, '${s['value']}', options, '${s['description'] ?? ''}')
        : await _ask(key, initial: '${s['value']}', hint: '${s['description'] ?? ''}');
    if (value == null || value == '${s['value']}') return;
    await _do({'cmd': 'set_setting', 'key': key, 'value': value},
        s['needs_restart'] == true ? '$key changed. It takes effect after a restart.' : '$key changed.');
  }

  // The picture is shrunk here, so the server only ever holds a small one.
  Future<void> _choosePicture() async {
    final messenger = ScaffoldMessenger.of(context);
    String? small;
    try {
      final file =
          await ImagePicker().pickImage(source: ImageSource.gallery, maxWidth: 1024, maxHeight: 1024);
      if (file == null) return;
      small = shrinkToProfilePicture(await file.readAsBytes());
    } catch (_) {
      messenger.showSnackBar(const SnackBar(content: Text('The picture could not be opened.')));
      return;
    }
    if (small == null) {
      messenger.showSnackBar(const SnackBar(content: Text('That file is not a picture Corded can read.')));
      return;
    }
    await _do({'cmd': 'set_setting', 'key': 'icon', 'value': small}, 'Server picture changed.');
  }

  String _uptime() {
    final started = (_status['started_at'] as num?)?.toInt() ?? 0;
    if (started == 0) return '';
    final d = DateTime.now().difference(DateTime.fromMillisecondsSinceEpoch(started));
    if (d.inDays > 0) return 'up ${d.inDays} d ${d.inHours % 24} h';
    if (d.inHours > 0) return 'up ${d.inHours} h ${d.inMinutes % 60} min';
    return 'up ${d.inMinutes} min';
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final channels = widget.state.store.roomsOf(_server.id).where((r) => r.kind == 'channel').toList();
    Widget heading(String text) => Padding(
          padding: const EdgeInsets.fromLTRB(16, 20, 16, 4),
          child: Text(text, style: theme.textTheme.labelLarge?.copyWith(color: theme.colorScheme.primary)),
        );
    return Scaffold(
      appBar: AppBar(title: Text('Manage ${_server.name.isEmpty ? 'server' : _server.name}')),
      body: ListView(children: [
        if (_server.can('manage_channels')) ...[
          heading('Channels'),
          for (final r in channels)
            ListTile(
              leading: const Icon(Icons.tag),
              title: Text(r.title.replaceFirst('#', '') +
                  (r.nsfw ? '  ·  NSFW' : '') +
                  (r.archived ? '  ·  archived' : '')),
              trailing: const Icon(Icons.more_vert),
              onTap: () => _channelActions(r),
            ),
          ListTile(leading: const Icon(Icons.add), title: const Text('New channel'), onTap: _newChannel),
        ],
        if (_server.can('manage_roles')) ...[
          heading('Roles'),
          ListTile(
            leading: const Icon(Icons.shield_outlined),
            title: const Text('Roles and permissions'),
            subtitle: Text(_server.roles.isEmpty ? 'Only "everyone" so far' : _server.roles.join(', ')),
            trailing: const Icon(Icons.chevron_right),
            onTap: () =>
                Navigator.push(context, MaterialPageRoute(builder: (_) => RolesScreen(state: widget.state))),
          ),
        ],
        if (_server.can('create_invite')) ...[
          heading('Invites'),
          ListTile(
            leading: const Icon(Icons.link),
            title: const Text('Make an invite link'),
            subtitle: const Text('Good for 7 days'),
            onTap: _invite,
          ),
        ],
        if (_server.can('manage_server')) ...[
          heading('Settings'),
          ListTile(
            leading: ServerIcon(_server),
            title: const Text('Server picture'),
            subtitle: Text(_server.icon.isEmpty ? 'None. Tap to choose one.' : 'Tap to change it.'),
            trailing: _server.icon.isEmpty
                ? null
                : IconButton(
                    tooltip: 'Remove the picture',
                    icon: const Icon(Icons.delete_outline),
                    onPressed: () =>
                        _do({'cmd': 'set_setting', 'key': 'icon', 'value': ''}, 'Server picture removed.'),
                  ),
            onTap: _choosePicture,
          ),
          for (final s in _settings.where((s) => s['key'] != 'icon'))
            ListTile(
              title: Text('${s['key']}: ${s['value']}'),
              subtitle: Text([
                '${s['description'] ?? ''}',
                if (s['owner_only'] == true) 'Owner only.',
                if (s['needs_restart'] == true) 'Needs a restart.',
              ].join(' ')),
              enabled: s['owner_only'] != true || _server.isOwner,
              onTap: () => _editSetting(s),
            ),
          heading('Server'),
          if (_status.isNotEmpty)
            ListTile(
              leading: const Icon(Icons.monitor_heart_outlined),
              title: Text('Version ${_status['version'] ?? '?'}, ${_uptime()}'),
              subtitle: Text('${_status['members'] ?? 0} members, ${_status['online'] ?? 0} online, '
                  '${(((_status['stored_bytes'] as num?) ?? 0) / 1024).round()} KB stored'),
            ),
          ListTile(
            leading: const Icon(Icons.restart_alt),
            title: const Text('Restart the server'),
            subtitle: const Text('Everyone is disconnected for a few seconds and reconnects'),
            onTap: () async {
              if (await _confirm(
                  'Restart the server?',
                  'Everyone is disconnected for a few seconds. Their apps reconnect by themselves.',
                  'Restart')) {
                await _do({'cmd': 'restart_server'}, 'The server is restarting.');
              }
            },
          ),
        ],
      ]),
    );
  }
}

/// A channel name as the server wants it: lower case, dashes for spaces, and
/// nothing but letters, digits, dashes and underscores.
String tidyChannelName(String typed) {
  var name = typed.trim().toLowerCase().replaceFirst(RegExp(r'^#+'), '');
  name = name.replaceAll(RegExp(r'\s+'), '-').replaceAll(RegExp(r'[^a-z0-9_-]'), '');
  name = name.replaceAll(RegExp(r'-{2,}'), '-').replaceAll(RegExp(r'^-+|-+$'), '');
  return name.length > 32 ? name.substring(0, 32) : name;
}
