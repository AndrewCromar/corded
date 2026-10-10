import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

/// Runs [action], showing its error (if any) at the bottom of the screen.
/// Returns true if it succeeded.
Future<bool> attempt(BuildContext context, Future<void> Function() action) async {
  final messenger = ScaffoldMessenger.of(context);
  try {
    await action();
    return true;
  } on CordedError catch (e) {
    messenger.showSnackBar(SnackBar(content: Text(sentence(e.message))));
    return false;
  }
}

/// The core's messages are lower-case fragments; show them as sentences.
String sentence(String s) {
  if (s.isEmpty) return s;
  final t = s[0].toUpperCase() + s.substring(1);
  return t.endsWith('.') || t.endsWith('?') || t.endsWith('!') ? t : '$t.';
}

/// A form page: a title, some fields, one main button.
class FormPage extends StatelessWidget {
  const FormPage({super.key, required this.title, required this.children, this.canGoBack = false});
  final String title;
  final List<Widget> children;
  final bool canGoBack;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text(title), automaticallyImplyLeading: canGoBack),
      body: SafeArea(
        child: Center(
          child: SingleChildScrollView(
            padding: const EdgeInsets.all(24),
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 420),
              child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: children),
            ),
          ),
        ),
      ),
    );
  }
}

/// A button that shows a spinner while its action runs.
class BusyButton extends StatefulWidget {
  const BusyButton({super.key, required this.label, required this.onPressed});
  final String label;
  final Future<void> Function() onPressed;

  @override
  State<BusyButton> createState() => _BusyButtonState();
}

class _BusyButtonState extends State<BusyButton> {
  bool _busy = false;

  @override
  Widget build(BuildContext context) {
    return FilledButton(
      onPressed: _busy
          ? null
          : () async {
              setState(() => _busy = true);
              try {
                await widget.onPressed();
              } finally {
                if (mounted) setState(() => _busy = false);
              }
            },
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 12),
        child: _busy
            ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2))
            : Text(widget.label),
      ),
    );
  }
}
