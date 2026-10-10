import 'dart:async';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'chat.dart';
import 'common.dart';

/// Looks through the messages kept on this device. Nothing is sent anywhere.
class SearchScreen extends StatefulWidget {
  const SearchScreen({super.key, required this.state, this.roomId});
  final AppState state;

  /// Search one chat only; all chats when null.
  final String? roomId;

  @override
  State<SearchScreen> createState() => _SearchScreenState();
}

class _SearchScreenState extends State<SearchScreen> {
  final _query = TextEditingController();
  Timer? _pause;
  List<Message> _results = const [];
  String _searched = '';
  String? _error;

  @override
  void dispose() {
    _pause?.cancel();
    _query.dispose();
    super.dispose();
  }

  // Search a moment after the typing stops.
  void _changed(String text) {
    _pause?.cancel();
    _pause = Timer(const Duration(milliseconds: 300), () => _search(text.trim()));
  }

  Future<void> _search(String text) async {
    if (text.length < 2) {
      setState(() {
        _results = const [];
        _searched = '';
        _error = null;
      });
      return;
    }
    try {
      final r = await widget.state.engine.command(
          {'cmd': 'search', 'text': text, 'limit': 80, if (widget.roomId != null) 'room_id': widget.roomId});
      if (!mounted || _query.text.trim() != text) return;
      setState(() {
        _results = [
          for (final e in (r['results'] as List? ?? const []))
            Message.fromJson((e as Map).cast<String, dynamic>())
        ];
        _searched = text;
        _error = null;
      });
    } on CordedError catch (e) {
      if (mounted) setState(() => _error = sentence(e.message));
    }
  }

  static String _when(int ms) {
    final t = DateTime.fromMillisecondsSinceEpoch(ms);
    String two(int n) => n.toString().padLeft(2, '0');
    return '${t.year}-${two(t.month)}-${two(t.day)} ${two(t.hour)}:${two(t.minute)}';
  }

  // The message with the words that were searched for picked out.
  Widget _snippet(BuildContext context, String body) {
    final style = Theme.of(context).textTheme.bodyMedium;
    final at = body.toLowerCase().indexOf(_searched.toLowerCase());
    if (at < 0) return Text(body, maxLines: 2, overflow: TextOverflow.ellipsis, style: style);
    final from = at > 40 ? at - 30 : 0;
    return Text.rich(
      TextSpan(style: style, children: [
        TextSpan(text: (from > 0 ? '…' : '') + body.substring(from, at)),
        TextSpan(
            text: body.substring(at, at + _searched.length),
            style: TextStyle(
                fontWeight: FontWeight.bold,
                backgroundColor: Theme.of(context).colorScheme.tertiaryContainer)),
        TextSpan(text: body.substring(at + _searched.length)),
      ]),
      maxLines: 2,
      overflow: TextOverflow.ellipsis,
    );
  }

  @override
  Widget build(BuildContext context) {
    final store = widget.state.store;
    final where = widget.roomId == null ? 'all chats' : (store.rooms[widget.roomId]?.title ?? 'this chat');
    return Scaffold(
      appBar: AppBar(
        title: TextField(
          controller: _query,
          autofocus: true,
          textInputAction: TextInputAction.search,
          onChanged: _changed,
          onSubmitted: (v) => _search(v.trim()),
          decoration: InputDecoration(hintText: 'Search $where', border: InputBorder.none),
        ),
      ),
      body: _error != null
          ? Center(child: Text(_error!))
          : _searched.isEmpty
              ? const Center(
                  child: Padding(
                    padding: EdgeInsets.all(24),
                    child: Text('Searches the messages kept on this phone. Nothing is sent to any server.',
                        textAlign: TextAlign.center),
                  ),
                )
              : _results.isEmpty
                  ? Center(child: Text('Nothing found for "$_searched".'))
                  : ListView.separated(
                      itemCount: _results.length,
                      separatorBuilder: (_, __) => const Divider(height: 1),
                      itemBuilder: (context, i) {
                        final m = _results[i];
                        final room = store.rooms[m.roomId];
                        return ListTile(
                          title: Text('${m.mine ? 'You' : m.sender}  ·  ${room?.title ?? 'a chat'}',
                              style: Theme.of(context).textTheme.labelLarge),
                          subtitle: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                            _snippet(context, m.body),
                            Text(_when(m.timestamp), style: Theme.of(context).textTheme.labelSmall),
                          ]),
                          isThreeLine: true,
                          onTap: () => Navigator.push(
                              context,
                              MaterialPageRoute(
                                  builder: (_) => ChatScreen(
                                      state: widget.state,
                                      roomId: m.roomId,
                                      threadRoot: m.threadRoot,
                                      jumpTo: m.id))),
                        );
                      },
                    ),
    );
  }
}
