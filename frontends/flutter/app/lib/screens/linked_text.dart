import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:url_launcher/url_launcher.dart';

/// One piece of a message: plain words, or a web address.
typedef TextPiece = ({String text, Uri? link});

final _channelWord = RegExp(r'(^|[^A-Za-z0-9_#-])#([A-Za-z0-9_-]+)');
final _address = RegExp(r'https?://[^\s<>]+', caseSensitive: false);

/// Splits a message into plain text and web addresses. Punctuation that ends
/// a sentence is left out of the address.
List<TextPiece> splitLinks(String text) {
  final pieces = <TextPiece>[];
  var at = 0;
  for (final m in _address.allMatches(text)) {
    var end = m.end;
    while (end > m.start && '.,;:!?)"\''.contains(text[end - 1])) {
      end--;
    }
    final uri = Uri.tryParse(text.substring(m.start, end));
    if (uri == null || uri.host.isEmpty) continue;
    if (m.start > at) pieces.add((text: text.substring(at, m.start), link: null));
    pieces.add((text: text.substring(m.start, end), link: uri));
    at = end;
  }
  if (at < text.length) pieces.add((text: text.substring(at), link: null));
  return pieces;
}

/// Message text in which web addresses can be tapped to open the browser.
class LinkedText extends StatefulWidget {
  const LinkedText(this.text,
      {super.key, this.style, required this.linkColor, this.channels = const {}, this.onChannel});
  final String text;

  /// Channel names (without the #) the person can open, and where each leads.
  final Map<String, String> channels;
  final void Function(String roomId)? onChannel;
  final TextStyle? style;
  final Color linkColor;

  @override
  State<LinkedText> createState() => _LinkedTextState();
}

class _LinkedTextState extends State<LinkedText> {
  final _recognizers = <TapGestureRecognizer>[];

  @override
  void dispose() {
    for (final r in _recognizers) {
      r.dispose();
    }
    super.dispose();
  }

  // Plain text with each "@name" picked out.
  List<TextSpan> _withMentions(String text) {
    final spans = <TextSpan>[];
    var at = 0;
    // "@name" and "#channel", in the order they appear.
    final marks = <(int, int, String?)>[
      for (final (start, end) in mentionSpans(text)) (start, end, null),
      for (final m in _channelWord.allMatches(text))
        if (widget.channels.containsKey(m.group(2)!.toLowerCase()))
          (m.start + m.group(1)!.length, m.end, widget.channels[m.group(2)!.toLowerCase()]),
    ]..sort((a, b) => a.$1.compareTo(b.$1));
    for (final (start, end, roomId) in marks) {
      if (start < at) continue;
      if (start > at) spans.add(TextSpan(text: text.substring(at, start)));
      TapGestureRecognizer? tap;
      if (roomId != null && widget.onChannel != null) {
        tap = TapGestureRecognizer()..onTap = () => widget.onChannel!(roomId);
        _recognizers.add(tap);
      }
      spans.add(TextSpan(
          text: text.substring(start, end),
          recognizer: tap,
          style: TextStyle(
              color: widget.linkColor,
              fontWeight: FontWeight.w600,
              decoration: roomId != null ? TextDecoration.underline : null,
              decorationColor: widget.linkColor)));
      at = end;
    }
    if (at < text.length) spans.add(TextSpan(text: text.substring(at)));
    return spans;
  }

  Future<void> _open(Uri uri) async {
    final messenger = ScaffoldMessenger.of(context);
    var opened = false;
    try {
      opened = await launchUrl(uri, mode: LaunchMode.externalApplication);
    } catch (_) {
      // Reported below.
    }
    if (!opened) messenger.showSnackBar(const SnackBar(content: Text('Could not open that link.')));
  }

  @override
  Widget build(BuildContext context) {
    for (final r in _recognizers) {
      r.dispose();
    }
    _recognizers.clear();
    final pieces = splitLinks(widget.text);
    if (pieces.every((p) => p.link == null) &&
        mentionSpans(widget.text).isEmpty &&
        !_channelWord.hasMatch(widget.text)) {
      return Text(widget.text, style: widget.style);
    }
    return Text.rich(
      TextSpan(style: widget.style, children: [
        for (final p in pieces)
          if (p.link == null)
            ..._withMentions(p.text)
          else
            TextSpan(
              text: p.text,
              style: TextStyle(
                  color: widget.linkColor,
                  decoration: TextDecoration.underline,
                  decorationColor: widget.linkColor),
              recognizer: () {
                final r = TapGestureRecognizer()..onTap = () => _open(p.link!);
                _recognizers.add(r);
                return r;
              }(),
            ),
      ]),
    );
  }
}
