import 'package:flutter/material.dart';

import '../app_state.dart';
import 'common.dart';

class UnlockScreen extends StatefulWidget {
  const UnlockScreen({super.key, required this.state});
  final AppState state;

  @override
  State<UnlockScreen> createState() => _UnlockScreenState();
}

class _UnlockScreenState extends State<UnlockScreen> {
  final _passphrase = TextEditingController();

  @override
  void initState() {
    super.initState();
    // Offer the fingerprint straight away; typing stays possible.
    if (widget.state.fingerprintUnlock) {
      WidgetsBinding.instance.addPostFrameCallback((_) => _fingerprint());
    }
  }

  Future<void> _fingerprint() async {
    final messenger = ScaffoldMessenger.of(context);
    final problem = await widget.state.unlockWithFingerprint();
    if (problem != null) messenger.showSnackBar(SnackBar(content: Text(problem)));
  }

  Future<void> _unlock() async {
    final ok = await attempt(context, () => widget.state.engine.unlock(_passphrase.text));
    if (!ok) _passphrase.clear();
  }

  @override
  Widget build(BuildContext context) {
    return FormPage(title: 'Unlock Corded', children: [
      TextField(
        controller: _passphrase,
        obscureText: true,
        autofocus: !widget.state.fingerprintUnlock,
        onSubmitted: (_) => _unlock(),
        decoration: const InputDecoration(labelText: 'Passphrase', border: OutlineInputBorder()),
      ),
      const SizedBox(height: 20),
      BusyButton(label: 'Unlock', onPressed: _unlock),
      if (widget.state.fingerprintUnlock) ...[
        const SizedBox(height: 12),
        OutlinedButton.icon(
          onPressed: _fingerprint,
          icon: const Icon(Icons.fingerprint),
          label: const Padding(padding: EdgeInsets.symmetric(vertical: 12), child: Text('Use fingerprint')),
        ),
      ],
    ]);
  }
}
