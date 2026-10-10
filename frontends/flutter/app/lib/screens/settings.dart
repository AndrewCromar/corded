import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_state.dart';
import 'common.dart';

/// Your own settings. The server's settings will join them here later.
class SettingsScreen extends StatefulWidget {
  const SettingsScreen({super.key, required this.state});
  final AppState state;

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  Map<String, dynamic> _settings = const {};

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final r = await widget.state.engine.command({'cmd': 'client_settings'});
    if (mounted) setState(() => _settings = (r['client_settings'] as Map).cast<String, dynamic>());
  }

  Future<void> _set(Map<String, dynamic> cmd) async {
    await attempt(context, () => widget.state.engine.command(cmd));
    await _load();
  }

  Future<void> _displayName() async {
    final controller = TextEditingController();
    final name = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Display name on this server'),
        content: TextField(
          controller: controller,
          autofocus: true,
          decoration: const InputDecoration(hintText: 'Leave empty to use your username'),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, controller.text.trim()), child: const Text('Save')),
        ],
      ),
    );
    if (name == null) return;
    await _set({
      'cmd': 'set_nickname',
      'nickname': name,
      if (widget.state.server != null) 'server_id': widget.state.server!.id,
    });
  }

  Future<void> _recoveryKey() async {
    final go = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Show your recovery key?'),
        content: const Text('Anyone who has this key can set up a device as you and read your messages. '
            'Only show it when nobody else can see your screen.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Show')),
        ],
      ),
    );
    if (go != true || !mounted) return;
    String key = '';
    if (!await attempt(context, () async {
      key = (await widget.state.engine.command({'cmd': 'get_recovery_key'}))['recovery_key'] as String;
    })) {
      return;
    }
    if (!mounted) return;
    await showDialog<void>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Recovery key'),
        content: SelectableText(key, style: const TextStyle(fontFamily: 'monospace', fontSize: 16)),
        actions: [
          TextButton(
              onPressed: () => Clipboard.setData(ClipboardData(text: key)), child: const Text('Copy')),
          FilledButton(onPressed: () => Navigator.pop(context), child: const Text('Done')),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final server = widget.state.server;
    final role = server == null
        ? ''
        : server.isOwner
            ? 'You own this server'
            : server.permissions.contains('administrator')
                ? 'You are an administrator'
                : 'You are a member';
    return Scaffold(
      appBar: AppBar(title: const Text('Settings')),
      body: ListView(children: [
        ListTile(
          leading: const Icon(Icons.person_outline),
          title: Text(widget.state.store.username),
          subtitle: const Text('Your username. It is the same on every server.'),
        ),
        ListTile(
          leading: const Icon(Icons.badge_outlined),
          title: const Text('Display name'),
          subtitle: Text(server == null ? 'Join a server first' : 'How you appear on ${server.name}'),
          onTap: server == null ? null : _displayName,
        ),
        SwitchListTile(
          secondary: const Icon(Icons.done_all),
          title: const Text('Read receipts'),
          subtitle: const Text('Let others see which messages you have read'),
          value: _settings['send_read_receipts'] != false,
          onChanged: (v) => _set({'cmd': 'set_read_receipts', 'enabled': v}),
        ),
        SwitchListTile(
          secondary: const Icon(Icons.history),
          title: const Text('Share earlier messages'),
          subtitle: const Text('Give newcomers the messages sent before they joined'),
          value: _settings['share_history'] != false,
          onChanged: (v) => _set({'cmd': 'set_history_sharing', 'enabled': v}),
        ),
        ListTile(
          leading: const Icon(Icons.key_outlined),
          title: const Text('Recovery key'),
          subtitle: const Text('For setting up another device as you'),
          onTap: _recoveryKey,
        ),
        const Divider(),
        if (server != null)
          ListTile(
            leading: const Icon(Icons.dns_outlined),
            title: Text(server.name.isEmpty ? server.address : server.name),
            subtitle: Text('${server.address}\n$role'),
            isThreeLine: true,
          ),
        ListTile(
          leading: const Icon(Icons.lock_outline),
          title: const Text('Lock'),
          subtitle: const Text('Ask for the passphrase again'),
          onTap: () async {
            final navigator = Navigator.of(context);
            await widget.state.lock();
            navigator.popUntil((r) => r.isFirst);
          },
        ),
        ListTile(
          leading: const Icon(Icons.info_outline),
          title: const Text('About'),
          subtitle: Text('${widget.state.engine.version}\nPrototype. Not yet audited; do not rely on it for secrets.'),
          isThreeLine: true,
        ),
      ]),
    );
  }
}
