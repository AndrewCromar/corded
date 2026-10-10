import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:url_launcher/url_launcher.dart';

/// One piece of a message: plain words, or a web address.
typedef TextPiece = ({String text, Uri? link});

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
  const LinkedText(this.text, {super.key, this.style, required this.linkColor});
  final String text;
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
    for (final (start, end) in mentionSpans(text)) {
      if (start > at) spans.add(TextSpan(text: text.substring(at, start)));
      spans.add(TextSpan(
          text: text.substring(start, end),
          style: TextStyle(color: widget.linkColor, fontWeight: FontWeight.w600)));
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
    if (pieces.every((p) => p.link == null) && mentionSpans(widget.text).isEmpty) {
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
