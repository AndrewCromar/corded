import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_state.dart';

/// Ctrl+K: type part of a chat's name and press Enter to go there. Chats with
/// something unread come first.
Future<void> showQuickSwitch(BuildContext context, AppState state) =>
    showDialog<void>(context: context, builder: (_) => _QuickSwitch(state: state));

class _QuickSwitch extends StatefulWidget {
  const _QuickSwitch({required this.state});
  final AppState state;

  @override
  State<_QuickSwitch> createState() => _QuickSwitchState();
}

class _QuickSwitchState extends State<_QuickSwitch> {
  final _typed = TextEditingController();
  int _at = 0;

  List<Room> get _found {
    final server = widget.state.server;
    final want = _typed.text.trim().toLowerCase().replaceFirst('#', '');
    final rooms = [
      for (final r in widget.state.store.rooms.values)
        if ((server == null || r.serverId == server.id) &&
            r.title.toLowerCase().contains(want) &&
            // Hidden and archived channels only when asked for by name.
            (want.isNotEmpty || !(r.nsfw || r.archived)))
          r
    ]..sort((a, b) {
        if ((a.unread > 0) != (b.unread > 0)) return a.unread > 0 ? -1 : 1;
        return b.lastActivity.compareTo(a.lastActivity);
      });
    return rooms.take(8).toList();
  }

  void _go(Room room) {
    Navigator.pop(context);
    widget.state.onOpenChat?.call(room.id, null);
  }

  @override
  void dispose() {
    _typed.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final found = _found;
    final at = found.isEmpty ? 0 : _at.clamp(0, found.length - 1);
    return Dialog(
      alignment: const Alignment(0, -0.6),
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 460),
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 4),
            child: Focus(
              onKeyEvent: (node, event) {
                if (event is KeyUpEvent) return KeyEventResult.ignored;
                if (event.logicalKey == LogicalKeyboardKey.arrowDown) {
                  setState(() => _at = at + 1);
                  return KeyEventResult.handled;
                }
                if (event.logicalKey == LogicalKeyboardKey.arrowUp) {
                  setState(() => _at = at - 1);
                  return KeyEventResult.handled;
                }
                return KeyEventResult.ignored;
              },
              child: TextField(
                controller: _typed,
                autofocus: true,
                decoration: const InputDecoration(
                    hintText: 'Go to a chat', prefixIcon: Icon(Icons.search), border: InputBorder.none),
                onChanged: (_) => setState(() => _at = 0),
                onSubmitted: (_) {
                  if (found.isNotEmpty) _go(found[at]);
                },
              ),
            ),
          ),
          const Divider(height: 1),
          for (final (i, room) in found.indexed)
            ListTile(
              dense: true,
              selected: i == at,
              leading: Icon(room.kind == 'channel'
                  ? (room.isTaskList ? Icons.checklist : Icons.tag)
                  : (room.kind == 'direct' ? Icons.person_outline : Icons.group_outlined)),
              title: Text(room.title, maxLines: 1, overflow: TextOverflow.ellipsis),
              trailing: room.unread > 0 ? Badge(label: Text('${room.unread}')) : null,
              onTap: () => _go(room),
            ),
          if (found.isEmpty) const ListTile(dense: true, title: Text('No chat is called that.')),
          const SizedBox(height: 8),
        ]),
      ),
    );
  }
}
