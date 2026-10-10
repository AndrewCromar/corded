import 'dart:io';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../platform.dart';
import '../updater.dart';
import '../app_state.dart';
import '../background.dart';
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

  // Asks GitHub for a newer release; if there is one, downloads it, has the
  // core check its signature, and installs it.
  Future<void> _checkForUpdates() async {
    final messenger = ScaffoldMessenger.of(context);
    final Update? update;
    try {
      update = await checkForUpdate();
    } catch (_) {
      messenger.showSnackBar(const SnackBar(content: Text('Could not reach GitHub to look for an update.')));
      return;
    }
    if (!mounted) return;
    if (update == null) {
      messenger.showSnackBar(const SnackBar(content: Text('You have the newest version.')));
      return;
    }
    final found = update;
    final go = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('${found.tag} is available'),
        content: Text(Platform.isAndroid
            ? 'Corded downloads it and checks that it is a genuine release. Android then asks you to confirm the install.'
            : 'Corded downloads it, checks that it is a genuine release, installs it and starts again.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Not now')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Update')),
        ],
      ),
    );
    if (go != true || !mounted) return;
    final progress = ValueNotifier<double?>(null);
    showDialog<void>(
      context: context,
      barrierDismissible: false,
      builder: (context) => AlertDialog(
        title: Text('Getting ${found.tag}'),
        content: ValueListenableBuilder<double?>(
            valueListenable: progress, builder: (context, value, _) => LinearProgressIndicator(value: value)),
      ),
    );
    try {
      final path = await downloadUpdate(widget.state.engine, found,
          progress: (got, total) => progress.value = total > 0 ? got / total : null);
      if (mounted) Navigator.pop(context);
      await installUpdate(path); // on a desktop the app ends here and comes back as the new one
    } on CordedError catch (e) {
      if (mounted) Navigator.pop(context);
      messenger.showSnackBar(SnackBar(content: Text(e.message)));
    } catch (_) {
      if (mounted) Navigator.pop(context);
      messenger.showSnackBar(const SnackBar(content: Text('The update could not be downloaded.')));
    }
  }

  static const _iconChannel = MethodChannel('org.corded.app/icon');
  static const _iconColours = {
    'indigo': Color(0xFF384379),
    'forest': Color(0xFF1E3B2A),
    'black': Color(0xFF151518),
    'plum': Color(0xFF5B2A5E),
    'teal': Color(0xFF124E55),
  };

  // The app carries one launcher entry per colour; this switches which is on.
  Future<void> _chooseIcon() async {
    final messenger = ScaffoldMessenger.of(context);
    String current = 'indigo';
    try {
      current = await _iconChannel.invokeMethod<String>('get') ?? current;
    } catch (_) {
      // Only Android can change its icon; elsewhere there is nothing to pick.
      messenger.showSnackBar(const SnackBar(content: Text('The icon cannot be changed on this device.')));
      return;
    }
    if (!mounted) return;
    final chosen = await showDialog<String>(
      context: context,
      builder: (context) => SimpleDialog(title: const Text('App icon'), children: [
        for (final e in _iconColours.entries)
          ListTile(
            leading: Container(
              width: 36,
              height: 36,
              decoration: BoxDecoration(color: e.value, borderRadius: BorderRadius.circular(9)),
            ),
            title: Text(e.key[0].toUpperCase() + e.key.substring(1)),
            trailing: e.key == current ? const Icon(Icons.check) : null,
            onTap: () => Navigator.pop(context, e.key),
          ),
        const Padding(
          padding: EdgeInsets.fromLTRB(24, 8, 24, 0),
          child: Text('Your home screen may take a moment to show the change, and a shortcut you placed '
              'there yourself may need to be put back.'),
        ),
      ]),
    );
    if (chosen == null || chosen == current) return;
    try {
      await _iconChannel.invokeMethod<String>('set', chosen);
      messenger.showSnackBar(const SnackBar(content: Text('App icon changed.')));
    } catch (_) {
      messenger.showSnackBar(const SnackBar(content: Text('The icon could not be changed.')));
    }
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
    // The page is grouped under a few headings, most-used things first.
    Widget heading(String text) => Padding(
          padding: const EdgeInsets.fromLTRB(16, 20, 16, 4),
          child: Text(text,
              style: Theme.of(context)
                  .textTheme
                  .titleSmall
                  ?.copyWith(color: Theme.of(context).colorScheme.primary)),
        );
    return Scaffold(
      appBar: AppBar(title: const Text('Settings')),
      body: ListView(children: [
        heading('You'),
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
        if (Background.supported || isDesktop) heading('Notifications'),
        if (Background.supported)
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
        // On a desktop the app itself announces messages while its window is not in front.
        if (widget.state.backgroundMode || isDesktop)
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
        heading('Privacy and security'),
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
        SwitchListTile(
          secondary: const Icon(Icons.link),
          title: const Text('Preview links you send'),
          subtitle: const Text('This device fetches the page and attaches a small card. '
              'People who read it contact nobody.'),
          value: widget.state.linkPreviews,
          onChanged: (v) async {
            await widget.state.setLinkPreviews(v);
            if (mounted) setState(() {});
          },
        ),
        if (widget.state.fingerprintAvailable)
          SwitchListTile(
            secondary: const Icon(Icons.fingerprint),
            title: const Text('Unlock with fingerprint'),
            subtitle: const Text('Open Corded with your fingerprint instead of typing the passphrase'),
            value: widget.state.fingerprintUnlock,
            onChanged: _setFingerprint,
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
        heading('Look and feel'),
        ListTile(
          leading: const Icon(Icons.add_reaction_outlined),
          title: const Text('Quick reactions'),
          subtitle: Text(widget.state.reactionBar.join('  ')),
          onTap: _reactionBar,
        ),
        if (Platform.isAndroid)
          ListTile(
            leading: const Icon(Icons.palette_outlined),
            title: const Text('App icon'),
            subtitle: const Text('The colour of the icon on your home screen'),
            onTap: _chooseIcon,
          ),
        if (server != null) heading('This server'),
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
            subtitle: const Text('Removes it and its chats from this device. Your account there stays.'),
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
        heading('This app'),
        ListTile(
          leading: const Icon(Icons.system_update_alt),
          title: const Text('Check for updates'),
          subtitle: Text('This is ${appVersion == 'dev' ? 'a build made by hand' : releaseOf(appVersion)}'),
          onTap: _checkForUpdates,
        ),
        ListTile(
          leading: const Icon(Icons.info_outline),
          title: const Text('About'),
          subtitle: Text(
              '${widget.state.engine.version}\nPrototype. Not yet audited; do not rely on it for secrets.'),
          isThreeLine: true,
        ),
        ListTile(
          leading: Icon(Icons.delete_forever_outlined, color: Theme.of(context).colorScheme.error),
          title: const Text('Sign out and erase this device'),
          subtitle: const Text('To start over, or to set this device up as a different person'),
          onTap: _signOut,
        ),
      ]),
    );
  }
}
