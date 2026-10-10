import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'common.dart';

/// Joins a server from an invite link or its address.
class AddServerScreen extends StatefulWidget {
  const AddServerScreen({super.key, required this.state, this.first = false});
  final AppState state;
  final bool first; // shown as the app's resting screen until a server is joined

  @override
  State<AddServerScreen> createState() => _AddServerScreenState();
}

class _AddServerScreenState extends State<AddServerScreen> {
  final _address = TextEditingController();
  final _invite = TextEditingController();

  Future<void> _join() async {
    final messenger = ScaffoldMessenger.of(context);
    final navigator = Navigator.of(context);
    final text = _address.text.trim();
    final Map<String, dynamic> cmd;
    if (text.startsWith('corded://')) {
      cmd = {'cmd': 'connect', 'link': text};
    } else {
      final colon = text.lastIndexOf(':');
      final host = colon > 0 ? text.substring(0, colon) : text;
      final port = colon > 0 ? int.tryParse(text.substring(colon + 1)) : 7443;
      if (host.isEmpty || port == null) {
        messenger.showSnackBar(
            const SnackBar(content: Text('Enter an invite link, or an address like 192.168.1.20:7443.')));
        return;
      }
      cmd = {'cmd': 'connect', 'host': host, 'port': port};
      if (_invite.text.trim().isNotEmpty) cmd['invite'] = _invite.text.trim();
    }
    final ok = await attempt(context, () async {
      await widget.state.engine.command(cmd).timeout(const Duration(seconds: 20),
          onTimeout: () => throw CordedError('timeout', 'The server did not answer. Check the address, and that this device can reach it'));
      await widget.state.store.refresh();
    });
    if (ok && !widget.first && navigator.canPop()) navigator.pop();
  }

  @override
  Widget build(BuildContext context) {
    return FormPage(title: 'Join a server', canGoBack: !widget.first, children: [
      Text(
        'Paste an invite link (it starts with corded://), or type the address of a server '
        'you were told about. You will join as ${widget.state.store.username}.',
        style: Theme.of(context).textTheme.bodyMedium,
      ),
      const SizedBox(height: 16),
      TextField(
        controller: _address,
        autocorrect: false,
        enableSuggestions: false,
        keyboardType: TextInputType.url,
        decoration: const InputDecoration(labelText: 'Invite link or address', border: OutlineInputBorder()),
      ),
      const SizedBox(height: 12),
      TextField(
        controller: _invite,
        autocorrect: false,
        decoration: const InputDecoration(
            labelText: 'Invite code (only if the server asks for one)', border: OutlineInputBorder()),
      ),
      const SizedBox(height: 20),
      BusyButton(label: 'Join', onPressed: _join),
    ]);
  }
}
