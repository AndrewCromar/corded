import 'dart:async';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_state.dart';

/// A bot's own page: what it says can be seen and changed about it.
///
/// Nothing here is known to the app in advance, and nothing to the server at
/// all. The app asks the bot, in the direct chat between this person and the
/// bot, what there is to manage (`m.bot.describe`); the bot answers with its
/// controls and their values as this person may see them
/// (`m.bot.description`); a change is sent back (`m.bot.set`, `m.bot.act`) and
/// the bot, which alone decides who may change what, says how it went
/// (`m.bot.result`) and describes itself afresh.
class ManageBotScreen extends StatefulWidget {
  const ManageBotScreen({super.key, required this.state, required this.member});
  final AppState state;
  final Member member;

  @override
  State<ManageBotScreen> createState() => _ManageBotScreenState();
}

class _ManageBotScreenState extends State<ManageBotScreen> {
  StreamSubscription<Map<String, dynamic>>? _sub;
  String? _room;
  Map<String, dynamic>? _page;
  String? _problem;
  final _waiting = <String>{}; // controls whose change the bot has not answered yet
  Timer? _patience;

  @override
  void initState() {
    super.initState();
    _begin();
  }

  @override
  void dispose() {
    _sub?.cancel();
    _patience?.cancel();
    super.dispose();
  }

  Future<void> _begin() async {
    try {
      final r = await widget.state.engine.command({'cmd': 'start_chat', 'username': widget.member.username});
      final room = (r['room'] as Map?)?['room_id'] as String?;
      if (room == null) throw CordedError('refused', 'The chat with this bot could not be opened.');
      _room = room;
      _sub = widget.state.engine.events.listen(_heard);
      await _ask('m.bot.describe', const {});
    } on CordedError catch (e) {
      if (mounted) setState(() => _problem = e.message);
    }
  }

  Future<void> _ask(String type, Map<String, Object?> content) async {
    _patience?.cancel();
    // A bot that is not running, or was written without this, says nothing.
    _patience = Timer(const Duration(seconds: 12), () {
      if (!mounted) return;
      setState(() {
        _waiting.clear();
        if (_page == null) {
          _problem = 'This bot did not answer. It may not be running, or it has nothing to manage from here.';
        }
      });
    });
    await widget.state.engine.command(
        {'cmd': 'send_event', 'room_id': _room, 'type': type, 'content': content, 'expires_in': 3600});
  }

  void _heard(Map<String, dynamic> event) {
    if (event['event'] != 'event_received') return;
    final data = (event['data'] as Map?)?.cast<String, dynamic>() ?? const {};
    if (data['room_id'] != _room || data['mine'] == true) return;
    final content = (data['content'] as Map?)?.cast<String, dynamic>() ?? const {};
    if (data['type'] == 'm.bot.description') {
      _patience?.cancel();
      if (mounted) {
        setState(() {
          _page = content;
          _problem = null;
          _waiting.clear();
        });
      }
    } else if (data['type'] == 'm.bot.result' && content['ok'] != true && mounted) {
      ScaffoldMessenger.of(context)
          .showSnackBar(SnackBar(content: Text('${content['message'] ?? 'The bot refused that.'}')));
    }
  }

  Future<void> _set(String key, Object? value) async {
    setState(() => _waiting.add(key));
    try {
      await _ask('m.bot.set', {'key': key, 'value': value});
    } on CordedError catch (e) {
      if (mounted) {
        setState(() => _waiting.remove(key));
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(e.message)));
      }
    }
  }

  Future<void> _press(Map<String, dynamic> c) async {
    final confirm = '${c['confirm'] ?? ''}';
    if (confirm.isNotEmpty) {
      final sure = await showDialog<bool>(
        context: context,
        builder: (context) => AlertDialog(
          title: Text('${c['label']}'),
          content: Text(confirm),
          actions: [
            TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
            FilledButton(onPressed: () => Navigator.pop(context, true), child: Text('${c['label']}')),
          ],
        ),
      );
      if (sure != true) return;
    }
    setState(() => _waiting.add('${c['key']}'));
    await _ask('m.bot.act', {'key': c['key']});
  }

  // A number or a line of text is typed in a small dialog.
  Future<void> _type(Map<String, dynamic> c, {required bool number}) async {
    final typed = TextEditingController(text: '${c['value'] ?? ''}');
    final range = number && c['min'] != null && c['max'] != null ? ' (${c['min']} to ${c['max']})' : '';
    final value = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('${c['label']}'),
        content: TextField(
          controller: typed,
          autofocus: true,
          keyboardType: number ? TextInputType.number : TextInputType.text,
          inputFormatters: number ? [FilteringTextInputFormatter.allow(RegExp(r'[0-9-]'))] : null,
          decoration: InputDecoration(helperText: '${c['help'] ?? ''}$range', helperMaxLines: 3),
          onSubmitted: (v) => Navigator.pop(context, v),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, typed.text), child: const Text('Save')),
        ],
      ),
    );
    if (value == null) return;
    if (number) {
      final n = int.tryParse(value.trim());
      if (n != null) await _set('${c['key']}', n);
    } else {
      await _set('${c['key']}', value);
    }
  }

  Future<void> _choose(Map<String, dynamic> c) async {
    final picked = await showDialog<Object>(
      context: context,
      builder: (context) => SimpleDialog(
        title: Text('${c['label']}'),
        children: [
          for (final option in (c['options'] as List? ?? const []))
            SimpleDialogOption(
              onPressed: () => Navigator.pop(context, option),
              child: Row(children: [
                Icon(option == c['value'] ? Icons.radio_button_checked : Icons.radio_button_unchecked,
                    size: 20),
                const SizedBox(width: 12),
                Text('$option'),
              ]),
            ),
        ],
      ),
    );
    if (picked != null && picked != c['value']) await _set('${c['key']}', picked);
  }

  Widget _control(Map<String, dynamic> c) {
    final theme = Theme.of(context);
    final key = '${c['key']}', label = '${c['label'] ?? key}', help = '${c['help'] ?? ''}';
    final may = c['can_change'] == true, busy = _waiting.contains(key);
    final subtitle = help.isEmpty ? null : Text(help);
    final spinner =
        busy ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)) : null;
    switch (c['type']) {
      case 'switch':
        return SwitchListTile(
          title: Text(label),
          subtitle: subtitle,
          value: c['value'] == true,
          onChanged: may && !busy ? (v) => _set(key, v) : null,
        );
      case 'number':
      case 'text':
        return ListTile(
          title: Text(label),
          subtitle: subtitle,
          trailing: spinner ?? Text('${c['value'] ?? ''}', style: theme.textTheme.titleMedium),
          enabled: may,
          onTap: may ? () => _type(c, number: c['type'] == 'number') : null,
        );
      case 'choice':
        return ListTile(
          title: Text(label),
          subtitle: subtitle,
          trailing: spinner ?? Text('${c['value'] ?? ''}', style: theme.textTheme.titleMedium),
          enabled: may,
          onTap: may ? () => _choose(c) : null,
        );
      case 'channels':
      case 'roles':
        // Several of a list: channels come as {id, title}, roles as names.
        final chosen = {for (final v in (c['value'] as List? ?? const [])) '$v'};
        final options = [
          for (final o in (c['options'] as List? ?? const []))
            o is Map ? (id: '${o['id']}', title: '${o['title']}') : (id: '$o', title: '$o')
        ];
        final named = [
          for (final o in options)
            if (chosen.contains(o.id)) o.title
        ];
        return ExpansionTile(
          title: Text(label),
          subtitle: Text(named.isEmpty ? (help.isEmpty ? 'None' : '$help\nNone') : named.join(', ')),
          trailing: spinner,
          shape: const Border(),
          collapsedShape: const Border(),
          children: [
            for (final o in options)
              CheckboxListTile(
                dense: true,
                title: Text(o.title),
                value: chosen.contains(o.id),
                onChanged: may && !busy
                    ? (on) => _set(key, [
                          for (final other in options)
                            if (other.id == o.id ? on == true : chosen.contains(other.id)) other.id
                        ])
                    : null,
              ),
          ],
        );
      case 'action':
        return ListTile(
          title: Text(label),
          subtitle: subtitle,
          trailing:
              spinner ?? FilledButton.tonal(onPressed: may ? () => _press(c) : null, child: Text(label)),
        );
      default: // "info", and kinds a newer bot may send that this app does not know: shown, not changed
        return ListTile(
            dense: true, title: Text(label), subtitle: subtitle, trailing: Text('${c['value'] ?? ''}'));
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final page = _page;
    final controls = [
      for (final c in (page?['controls'] as List? ?? const []))
        if (c is Map) c.cast<String, dynamic>()
    ];
    final mayAnything = controls.any((c) => c['can_change'] == true);
    return Scaffold(
      appBar: AppBar(title: Text('Manage ${widget.member.displayName}')),
      body: _problem != null
          ? Center(
              child: Padding(
                  padding: const EdgeInsets.all(24), child: Text(_problem!, textAlign: TextAlign.center)))
          : page == null
              ? const Center(child: CircularProgressIndicator())
              : ListView(children: [
                  ListTile(
                    title: Text('${page['title'] ?? widget.member.displayName}',
                        style: theme.textTheme.titleLarge),
                    subtitle: Text([
                      if ('${page['about'] ?? ''}'.isNotEmpty) '${page['about']}',
                      if ('${page['version'] ?? ''}'.isNotEmpty) 'Version ${page['version']}',
                    ].join('\n')),
                  ),
                  if (!mayAnything)
                    Padding(
                      padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                      child: Text('You can look, but changing this bot is for those it allows.',
                          style: theme.textTheme.bodySmall?.copyWith(color: theme.colorScheme.outline)),
                    ),
                  const Divider(height: 1),
                  for (final c in controls) _control(c),
                  const SizedBox(height: 24),
                ]),
    );
  }
}
