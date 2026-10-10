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
        autofocus: true,
        onSubmitted: (_) => _unlock(),
        decoration: const InputDecoration(labelText: 'Passphrase', border: OutlineInputBorder()),
      ),
      const SizedBox(height: 20),
      BusyButton(label: 'Unlock', onPressed: _unlock),
    ]);
  }
}
