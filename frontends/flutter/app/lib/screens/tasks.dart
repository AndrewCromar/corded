import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'chat.dart';
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

  CordedStore get _store => widget.state.store;

  Future<void> _react(Message task, String key) =>
      attempt(context, () => _store.toggleReaction(widget.room.id, task.id, key));

  // A task is talked over in a thread that hangs off it.
  void _discuss(Message task) => Navigator.push(
      context,
      MaterialPageRoute(
          builder: (_) => ChatScreen(state: widget.state, roomId: widget.room.id, threadRoot: task.id)));

  void _actions(Message task) {
    final canManage = widget.state.server?.permissions.contains('manage_messages') ?? false;
    showModalBottomSheet<void>(
      context: context,
      showDragHandle: true,
      builder: (sheet) => SafeArea(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          Row(mainAxisAlignment: MainAxisAlignment.spaceEvenly, children: [
            for (final key in widget.state.reactionBar)
              Flexible(
                child: IconButton(
                  icon: Text(key, style: const TextStyle(fontSize: 24)),
                  onPressed: () {
                    Navigator.pop(sheet);
                    _react(task, key);
                  },
                ),
              ),
          ]),
          ListTile(
            leading: const Icon(Icons.forum_outlined),
            title: Text(
                _store.threadCount(widget.room.id, task) > 0 ? 'Open the discussion' : 'Discuss this task'),
            onTap: () {
              Navigator.pop(sheet);
              _discuss(task);
            },
          ),
          if (task.mine)
            ListTile(
              leading: const Icon(Icons.edit_outlined),
              title: const Text('Reword'),
              onTap: () {
                Navigator.pop(sheet);
                _reword(task);
              },
            ),
          if (task.mine || canManage)
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
    final reactions = _store.reactions(widget.room.id, task.id);
    final replies = _store.threadCount(widget.room.id, task);
    return InkWell(
      onLongPress: () => _actions(task),
      onSecondaryTap: () => _actions(task),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(4, 2, 4, 2),
        child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Checkbox(
              value: task.taskDone,
              // Ticking is writing: not in an archived channel, nor one this person may only read.
              onChanged:
                  widget.room.archived || !widget.room.canSend ? null : (v) => _tick(task, v ?? false)),
          Expanded(
            child: Padding(
              padding: const EdgeInsets.only(top: 10, bottom: 6),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(task.body,
                    // Done is shown by the ticked box, the grey and the place in the list.
                    style: theme.textTheme.bodyLarge
                        ?.copyWith(color: task.taskDone ? theme.colorScheme.outline : null)),
                Text(note, style: theme.textTheme.labelSmall),
                if (reactions.isNotEmpty)
                  Wrap(spacing: 4, children: [
                    for (final e in reactions.entries)
                      FilterChip(
                        visualDensity: VisualDensity.compact,
                        showCheckmark: false,
                        selected: _store.myReaction(widget.room.id, task.id, e.key) != null,
                        label: Text('${e.key} ${e.value}'),
                        onSelected: (_) => _react(task, e.key),
                      ),
                  ]),
              ]),
            ),
          ),
          // The discussion under a task, with how many messages it holds.
          TextButton.icon(
            onPressed: () => _discuss(task),
            icon: Icon(replies > 0 ? Icons.forum : Icons.forum_outlined, size: 18),
            label: Text(replies > 0 ? '$replies' : ''),
          ),
        ]),
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
                    // Finished tasks fold away, so the page shows what is left to do.
                    if (done.isNotEmpty)
                      ExpansionTile(
                        key: PageStorageKey('done-${widget.room.id}'),
                        title: Text('Done (${done.length})',
                            style: theme.textTheme.titleSmall?.copyWith(color: theme.colorScheme.primary)),
                        shape: const Border(),
                        collapsedShape: const Border(),
                        children: [for (final t in done) _row(t)],
                      ),
                  ]),
          ),
          if (!widget.room.archived && widget.room.canSend)
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
