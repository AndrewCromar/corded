import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'common.dart';

/// A task-list channel: rows to tick off. Anyone who may write in the channel
/// can add a task and tick or untick any task; the newest tick decides.
class TasksView extends StatefulWidget {
  const TasksView({super.key, required this.state, required this.room, required this.tasks});
  final AppState state;
  final Room room;
  final List<Message> tasks;

  @override
  State<TasksView> createState() => _TasksViewState();
}

class _TasksViewState extends State<TasksView> {
  final _input = TextEditingController();

  @override
  void dispose() {
    _input.dispose();
    super.dispose();
  }

  Future<void> _add() async {
    final text = _input.text.trim();
    if (text.isEmpty) return;
    _input.clear();
    await attempt(context,
        () => widget.state.engine.command({'cmd': 'add_task', 'room_id': widget.room.id, 'body': text}));
  }

  Future<void> _tick(Message task, bool done) => attempt(
      context,
      () => widget.state.engine
          .command({'cmd': 'set_task_done', 'room_id': widget.room.id, 'event_id': task.id, 'done': done}));

  Future<void> _reword(Message task) async {
    final controller = TextEditingController(text: task.body);
    final text = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Reword the task'),
        content: TextField(controller: controller, autofocus: true, maxLines: null),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, controller.text.trim()), child: const Text('Save')),
        ],
      ),
    );
    if (text == null || text.isEmpty || text == task.body || !mounted) return;
    await attempt(
        context,
        () => widget.state.engine
            .command({'cmd': 'edit_event', 'room_id': widget.room.id, 'event_id': task.id, 'body': text}));
  }

  void _actions(Message task) {
    final canManage = widget.state.server?.permissions.contains('manage_messages') ?? false;
    if (!task.mine && !canManage) return;
    showModalBottomSheet<void>(
      context: context,
      showDragHandle: true,
      builder: (sheet) => SafeArea(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          if (task.mine)
            ListTile(
              leading: const Icon(Icons.edit_outlined),
              title: const Text('Reword'),
              onTap: () {
                Navigator.pop(sheet);
                _reword(task);
              },
            ),
          ListTile(
            leading: const Icon(Icons.delete_outline),
            title: const Text('Delete'),
            onTap: () {
              Navigator.pop(sheet);
              attempt(
                  context,
                  () => widget.state.engine
                      .command({'cmd': 'delete_event', 'room_id': widget.room.id, 'event_id': task.id}));
            },
          ),
        ]),
      ),
    );
  }

  Widget _row(Message task) {
    final theme = Theme.of(context);
    final note = task.taskDone
        ? (task.taskDoneBy.isEmpty ? 'Done' : 'Done by ${task.taskDoneBy}')
        : 'Added by ${task.mine ? 'you' : task.sender}';
    return InkWell(
      onLongPress: () => _actions(task),
      child: CheckboxListTile(
        value: task.taskDone,
        controlAffinity: ListTileControlAffinity.leading,
        enabled: !widget.room.archived,
        onChanged: (v) => _tick(task, v ?? false),
        title: Text(task.body,
            style: task.taskDone
                ? TextStyle(decoration: TextDecoration.lineThrough, color: theme.colorScheme.outline)
                : null),
        subtitle: Text(note, style: theme.textTheme.labelSmall),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final open = widget.tasks.where((t) => !t.taskDone).toList();
    final done = widget.tasks.where((t) => t.taskDone).toList();
    return Scaffold(
      appBar: AppBar(
        title:
            Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
          Text(widget.room.title.replaceFirst('#', '')),
          Text(
              widget.tasks.isEmpty
                  ? 'Task list'
                  : '${done.length} of ${widget.tasks.length} done${widget.room.archived ? ' · archived' : ''}',
              style: theme.textTheme.labelMedium),
        ]),
      ),
      body: SafeArea(
        child: Column(children: [
          Expanded(
            child: widget.tasks.isEmpty
                ? const Center(child: Text('Nothing to do yet. Add the first task below.'))
                : ListView(children: [
                    for (final t in open) _row(t),
                    if (done.isNotEmpty)
                      Padding(
                        padding: const EdgeInsets.fromLTRB(16, 16, 16, 4),
                        child: Text('Done',
                            style: theme.textTheme.titleSmall?.copyWith(color: theme.colorScheme.primary)),
                      ),
                    for (final t in done) _row(t),
                  ]),
          ),
          if (!widget.room.archived)
            Padding(
              padding: const EdgeInsets.fromLTRB(12, 4, 4, 8),
              child: Row(children: [
                Expanded(
                  child: TextField(
                    controller: _input,
                    textCapitalization: TextCapitalization.sentences,
                    onSubmitted: (_) => _add(),
                    decoration: InputDecoration(
                      hintText: 'Add a task',
                      border: OutlineInputBorder(borderRadius: BorderRadius.circular(24)),
                      contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
                    ),
                  ),
                ),
                const SizedBox(width: 4),
                IconButton.filled(onPressed: _add, tooltip: 'Add', icon: const Icon(Icons.add)),
              ]),
            ),
        ]),
      ),
    );
  }
}
