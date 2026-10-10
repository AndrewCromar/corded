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

/// One thing that can be done from a menu.
typedef Choice = ({IconData icon, String title, String? subtitle, VoidCallback run});

/// A menu of things to do, shown the way the device expects: where the pointer
/// is when a mouse asked for it ([at] given), sliding up from the bottom
/// otherwise. [reactions] puts a row of emoji above the choices.
void showChoices(
  BuildContext context,
  List<Choice> choices, {
  Offset? at,
  List<String> reactions = const [],
  void Function(String key)? onReact,
  VoidCallback? onOtherReaction,
}) {
  Widget emoji(BuildContext menu, {required bool small}) =>
      Row(mainAxisAlignment: MainAxisAlignment.spaceEvenly, children: [
        for (final key in reactions)
          Flexible(
            child: IconButton(
              icon: Text(key, style: TextStyle(fontSize: small ? 20 : 24)),
              onPressed: () {
                Navigator.pop(menu);
                onReact?.call(key);
              },
            ),
          ),
        if (onOtherReaction != null)
          Flexible(
            child: IconButton(
              tooltip: 'Another emoji',
              icon: const Icon(Icons.add_circle_outline),
              onPressed: () {
                Navigator.pop(menu);
                onOtherReaction();
              },
            ),
          ),
      ]);

  if (at != null) {
    // Measured within whichever part of the window this menu belongs to.
    final overlay = Navigator.of(context).overlay!.context.findRenderObject()! as RenderBox;
    showMenu<VoidCallback>(
      context: context,
      position: RelativeRect.fromRect(overlay.globalToLocal(at) & Size.zero, Offset.zero & overlay.size),
      items: [
        if (reactions.isNotEmpty) ...[
          _MenuRow(child: Builder(builder: (menu) => emoji(menu, small: true))),
          const PopupMenuDivider(),
        ],
        for (final c in choices)
          PopupMenuItem(
            value: c.run,
            child: Row(children: [
              Icon(c.icon, size: 20),
              const SizedBox(width: 12),
              Flexible(child: Text(c.title)),
            ]),
          ),
      ],
    ).then((run) => run?.call());
    return;
  }
  showModalBottomSheet<void>(
    context: context,
    showDragHandle: true,
    builder: (sheet) => SafeArea(
      child: SingleChildScrollView(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          if (reactions.isNotEmpty) emoji(sheet, small: false),
          for (final c in choices)
            ListTile(
              leading: Icon(c.icon),
              title: Text(c.title),
              subtitle: c.subtitle == null ? null : Text(c.subtitle!),
              onTap: () {
                Navigator.pop(sheet);
                c.run();
              },
            ),
        ]),
      ),
    ),
  );
}

// A row in a pointer menu that is not itself a choice.
class _MenuRow extends PopupMenuEntry<VoidCallback> {
  const _MenuRow({required this.child});
  final Widget child;

  @override
  double get height => 48;

  @override
  bool represents(VoidCallback? value) => false;

  @override
  State<_MenuRow> createState() => _MenuRowState();
}

class _MenuRowState extends State<_MenuRow> {
  @override
  Widget build(BuildContext context) => SizedBox(width: 260, child: widget.child);
}
