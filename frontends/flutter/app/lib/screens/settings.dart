import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_state.dart';
import 'common.dart';
import 'devices.dart';
import 'profile.dart';
import 'scan.dart';
import 'server.dart';

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

  static const _statuses = {
    'auto': ('Automatic', 'Online while you use the app, away when you do not'),
    'dnd': ('Do not disturb', 'Others see you are busy, and this phone shows no message notifications'),
    'invisible': ('Invisible', 'Others see you as offline; you still get everything'),
  };

  Future<void> _status() async {
    final current = '${_settings['presence'] ?? 'auto'}';
    final picked = await showDialog<String>(
      context: context,
      builder: (context) => SimpleDialog(
        title: const Text('Status'),
        children: [
          for (final s in _statuses.entries)
            ListTile(
              leading: Icon(s.key == current ? Icons.radio_button_checked : Icons.radio_button_unchecked),
              title: Text(s.value.$1),
              subtitle: Text(s.value.$2),
              onTap: () => Navigator.pop(context, s.key),
            ),
        ],
      ),
    );
    if (picked == null || picked == current) return;
    await _set({'cmd': 'set_presence', 'status': picked});
    widget.state.setDoNotDisturb(picked == 'dnd');
  }

  // Signing out cannot be undone, so it takes two deliberate steps.
  Future<void> _signOut() async {
    final navigator = Navigator.of(context);
    final name = widget.state.store.username;
    final understood = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        icon: Icon(Icons.warning_amber_rounded, color: Theme.of(context).colorScheme.error, size: 36),
        title: const Text('Sign out and erase this device?'),
        content: SingleChildScrollView(
          child: Text('This deletes everything Corded keeps on this phone:\n\n'
              '• your identity keys for "$name" on this device\n'
              '• every message stored here\n'
              '• the list of servers you joined\n\n'
              'Your account on each server stays, and other people keep their copies of your messages.\n\n'
              'You can only come back as "$name" with your recovery key (Settings > Recovery key) '
              'or from another device that is still signed in. Without one of those, "$name" is gone for good '
              'and nobody can restore it.\n\n'
              'If you have not saved the recovery key and might want this identity again, cancel and save it first.'),
        ),
        actions: [
          FilledButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.pop(context, true), child: const Text('Continue')),
        ],
      ),
    );
    if (understood != true || !mounted) return;
    final controller = TextEditingController();
    final typed = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Last check'),
        content:
            Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text('Type your username, $name, to erase this device. This cannot be undone.'),
          const SizedBox(height: 12),
          TextField(controller: controller, autofocus: true, autocorrect: false),
        ]),
        actions: [
          FilledButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          TextButton(
            onPressed: () => Navigator.pop(context, controller.text.trim()),
            child: Text('Erase', style: TextStyle(color: Theme.of(context).colorScheme.error)),
          ),
        ],
      ),
    );
    if (typed == null || !mounted) return;
    if (typed.toLowerCase() != name.toLowerCase()) {
      ScaffoldMessenger.of(context)
          .showSnackBar(const SnackBar(content: Text('That is not your username, so nothing was erased.')));
      return;
    }
    navigator.popUntil((r) => r.isFirst);
    await widget.state.signOut();
  }

  Future<void> _setFingerprint(bool on) async {
    final messenger = ScaffoldMessenger.of(context);
    if (!on) {
      await widget.state.disableFingerprint();
      if (mounted) setState(() {});
      return;
    }
    final controller = TextEditingController();
    final passphrase = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Unlock with fingerprint'),
        content:
            Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
          const Text("Type your passphrase once more. It is then kept in this phone's secure hardware and "
              'released only by your fingerprint. The passphrase itself keeps working.'),
          const SizedBox(height: 12),
          TextField(
            controller: controller,
            autofocus: true,
            obscureText: true,
            decoration: const InputDecoration(labelText: 'Passphrase'),
            onSubmitted: (v) => Navigator.pop(context, v),
          ),
        ]),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, controller.text), child: const Text('Turn on')),
        ],
      ),
    );
    if (passphrase == null || passphrase.isEmpty) return;
    try {
      await widget.state.enableFingerprint(passphrase);
      messenger.showSnackBar(const SnackBar(content: Text('Fingerprint unlock is on.')));
    } on CordedError {
      messenger.showSnackBar(const SnackBar(content: Text('That is not your passphrase.')));
    } catch (_) {
      messenger.showSnackBar(
          const SnackBar(content: Text('The fingerprint check did not finish, so nothing changed.')));
    }
    if (mounted) setState(() {});
  }

  Future<void> _reactionBar() async {
    final controller = TextEditingController(text: widget.state.reactionBar.join(' '));
    final text = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Quick reactions'),
        content: Column(mainAxisSize: MainAxisSize.min, children: [
          TextField(
            controller: controller,
            autofocus: true,
            style: const TextStyle(fontSize: 24),
            decoration: const InputDecoration(helperText: 'Up to 8. The first is sent by a double-tap.'),
          ),
        ]),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, ''), child: const Text('Reset')),
          FilledButton(onPressed: () => Navigator.pop(context, controller.text), child: const Text('Save')),
        ],
      ),
    );
    if (text == null) return;
    await widget.state.setReactionBar(AppState.parseReactions(text));
    if (mounted) setState(() {});
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
        content: SingleChildScrollView(
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            // On the new device: "I have another device" > scan.
            CodeToScan(recoveryCode(widget.state.store.username, key), size: 200),
            const SizedBox(height: 12),
            SelectableText(key, style: const TextStyle(fontFamily: 'monospace', fontSize: 16)),
          ]),
        ),
        actions: [
          TextButton(onPressed: () => Clipboard.setData(ClipboardData(text: key)), child: const Text('Copy')),
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
          leading: const Icon(Icons.circle_outlined),
          title: const Text('Status'),
          subtitle: Text(_statuses['${_settings['presence'] ?? 'auto'}']?.$1 ?? 'Automatic'),
          onTap: _status,
        ),
        ListTile(
          leading: const Icon(Icons.badge_outlined),
          title: const Text('Your profile'),
          subtitle: const Text('Display name, birthday, links and more. Only people you chat with see it'),
          onTap: () => Navigator.push(
              context, MaterialPageRoute(builder: (_) => EditProfileScreen(state: widget.state))),
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
        if (widget.state.fingerprintAvailable)
          SwitchListTile(
            secondary: const Icon(Icons.fingerprint),
            title: const Text('Unlock with fingerprint'),
            subtitle: const Text('Open Corded with your fingerprint instead of typing the passphrase'),
            value: widget.state.fingerprintUnlock,
            onChanged: _setFingerprint,
          ),
        SwitchListTile(
          secondary: const Icon(Icons.notifications_active_outlined),
          title: const Text('Stay connected in the background'),
          subtitle: const Text('Get a notification for each new message while the app is not on screen. '
              'Android shows a permanent notification while this is on, and it uses some battery.'),
          isThreeLine: true,
          value: widget.state.backgroundMode,
          onChanged: (on) async {
            final messenger = ScaffoldMessenger.of(context);
            final problem = await widget.state.setBackgroundMode(on);
            if (problem != null) messenger.showSnackBar(SnackBar(content: Text(problem)));
            if (mounted) setState(() {});
          },
        ),
        if (widget.state.backgroundMode)
          SwitchListTile(
            secondary: const Icon(Icons.visibility_outlined),
            title: const Text('Show message text in notifications'),
            subtitle: const Text('Off: a notification says who wrote and where, not what'),
            value: widget.state.showMessageText,
            onChanged: (v) async {
              await widget.state.setShowMessageText(v);
              if (mounted) setState(() {});
            },
          ),
        ListTile(
          leading: const Icon(Icons.add_reaction_outlined),
          title: const Text('Quick reactions'),
          subtitle: Text(widget.state.reactionBar.join('  ')),
          onTap: _reactionBar,
        ),
        if (server != null)
          ListTile(
            leading: const Icon(Icons.devices_outlined),
            title: const Text('Your devices'),
            subtitle: const Text('See what is signed in as you, and sign a device out'),
            onTap: () => Navigator.push(
                context, MaterialPageRoute(builder: (_) => DevicesScreen(state: widget.state))),
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
        if (server != null &&
            (server.can('manage_channels') ||
                server.can('manage_roles') ||
                server.can('create_invite') ||
                server.can('manage_server')))
          ListTile(
            leading: const Icon(Icons.tune),
            title: const Text('Manage this server'),
            subtitle: const Text('Channels, roles, invites, settings, restart'),
            onTap: () =>
                Navigator.push(context, MaterialPageRoute(builder: (_) => ServerScreen(state: widget.state))),
          ),
        if (server != null)
          ListTile(
            leading: const Icon(Icons.logout),
            title: const Text('Leave this server'),
            subtitle: const Text('Removes it and its chats from this phone. Your account there stays.'),
            onTap: () async {
              final navigator = Navigator.of(context);
              final leave = await showDialog<bool>(
                context: context,
                builder: (context) => AlertDialog(
                  title: Text('Leave ${server.name.isEmpty ? server.address : server.name}?'),
                  content: const Text('The server and its chats are removed from this device. '
                      'Your account stays on the server, so you can join again later as the same person.'),
                  actions: [
                    TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
                    FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Leave')),
                  ],
                ),
              );
              if (leave != true || !context.mounted) return;
              if (await attempt(context,
                  () => widget.state.engine.command({'cmd': 'forget_server', 'server_id': server.id}))) {
                navigator.popUntil((r) => r.isFirst);
              }
            },
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
          leading: Icon(Icons.delete_forever_outlined, color: Theme.of(context).colorScheme.error),
          title: const Text('Sign out and erase this device'),
          subtitle: const Text('To start over, or to set this phone up as a different person'),
          onTap: _signOut,
        ),
        ListTile(
          leading: const Icon(Icons.info_outline),
          title: const Text('About'),
          subtitle: Text(
              '${widget.state.engine.version}\nPrototype. Not yet audited; do not rely on it for secrets.'),
          isThreeLine: true,
        ),
      ]),
    );
  }
}
