import '../platform.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'common.dart';
import 'scan.dart';

/// First run: become someone new, or set this device up as an existing person.
class WelcomeScreen extends StatefulWidget {
  const WelcomeScreen({super.key, required this.state});
  final AppState state;

  @override
  State<WelcomeScreen> createState() => _WelcomeScreenState();
}

class _WelcomeScreenState extends State<WelcomeScreen> {
  final _username = TextEditingController();
  final _passphrase = TextEditingController();
  final _again = TextEditingController();
  final _recoveryKey = TextEditingController();
  bool _existing = false;

  Future<void> _submit() async {
    final messenger = ScaffoldMessenger.of(context);
    final name = _username.text.trim().toLowerCase();
    String? problem;
    if (!RegExp(r'^[a-z0-9_-]{1,32}$').hasMatch(name)) {
      problem = 'A username is 1 to 32 characters: a to z, 0 to 9, _ and -.';
    } else if (_passphrase.text.length < 8) {
      problem = 'Use a passphrase of at least 8 characters.';
    } else if (_passphrase.text != _again.text) {
      problem = 'The two passphrases do not match.';
    } else if (_existing && _recoveryKey.text.trim().isEmpty) {
      problem = 'Enter the recovery key from your other device.';
    }
    if (problem != null) {
      messenger.showSnackBar(SnackBar(content: Text(problem)));
      return;
    }
    await attempt(context, () async {
      final engine = widget.state.engine;
      if (_existing) {
        await engine.restoreVault(_passphrase.text, name, _recoveryKey.text.trim());
      } else {
        await engine.createVault(_passphrase.text, name);
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return FormPage(title: 'Welcome to Corded', children: [
      SegmentedButton<bool>(
        segments: const [
          ButtonSegment(value: false, label: Text("I'm new")),
          ButtonSegment(value: true, label: Text('I have another device')),
        ],
        selected: {_existing},
        onSelectionChanged: (s) => setState(() => _existing = s.first),
      ),
      const SizedBox(height: 16),
      Text(
        _existing
            ? 'Use the same username as on your other device, and its recovery key '
                '(type /recovery-key there). This device gets its own passphrase.'
            : 'Pick the name others will know you by. Your messages are stored on this '
                'device, locked with the passphrase you choose. It cannot be reset.',
        style: theme.textTheme.bodyMedium,
      ),
      const SizedBox(height: 16),
      TextField(
        controller: _username,
        autocorrect: false,
        decoration: const InputDecoration(labelText: 'Username', border: OutlineInputBorder()),
      ),
      if (_existing && isPhone) ...[
        const SizedBox(height: 12),
        OutlinedButton.icon(
          icon: const Icon(Icons.qr_code_scanner),
          label: const Padding(
              padding: EdgeInsets.symmetric(vertical: 10), child: Text('Scan the code on your other phone')),
          onPressed: () async {
            final messenger = ScaffoldMessenger.of(context);
            final text = await scanCode(context, title: 'Scan your recovery code');
            if (text == null || !mounted) return;
            final found = parseRecoveryCode(text.trim());
            if (found == null) {
              messenger.showSnackBar(const SnackBar(
                  content: Text(
                      'That is not a Corded recovery code. On the other phone: Settings > Recovery key.')));
              return;
            }
            setState(() {
              _username.text = found.username;
              _recoveryKey.text = found.key;
            });
          },
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _recoveryKey,
          autocorrect: false,
          enableSuggestions: false,
          minLines: 2,
          maxLines: 3,
          decoration: const InputDecoration(labelText: 'Recovery key', border: OutlineInputBorder()),
        ),
      ],
      const SizedBox(height: 12),
      TextField(
        controller: _passphrase,
        obscureText: true,
        decoration: const InputDecoration(labelText: 'Passphrase', border: OutlineInputBorder()),
      ),
      const SizedBox(height: 12),
      TextField(
        controller: _again,
        obscureText: true,
        decoration: const InputDecoration(labelText: 'Passphrase again', border: OutlineInputBorder()),
      ),
      const SizedBox(height: 20),
      BusyButton(label: _existing ? 'Set up this device' : 'Create', onPressed: _submit),
    ]);
  }
}
