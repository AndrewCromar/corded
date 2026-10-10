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

// ---------------------------------------------------------------- styling
// Messages are plain text. The common marks are drawn as styling, the way
// Discord does: **bold**, *italic* or _italic_, ~~strike~~, `code`,
// ||spoiler||, a line starting with "> " as a quote, and ``` fences for a
// block of code. A client that does not know them just shows the characters.

/// A run of text with the styles that apply to it.
typedef StyledRun = ({String text, bool bold, bool italic, bool strike, bool code, bool spoiler});

final _inline = RegExp(r'(`[^`\n]+`)|(\*\*(?=\S)(.+?)(?<=\S)\*\*)|(~~(?=\S)(.+?)(?<=\S)~~)|(\|\|(.+?)\|\|)'
    r'|(\*(?=\S)([^*\n]+?)(?<=\S)\*)|((?<![A-Za-z0-9])_(?=\S)([^_\n]+?)(?<=\S)_(?![A-Za-z0-9]))');

/// Breaks one stretch of text into styled runs. Styles nest, except inside code.
List<StyledRun> styleRuns(String text,
    {bool bold = false, bool italic = false, bool strike = false, bool spoiler = false}) {
  final runs = <StyledRun>[];
  void plain(String t) {
    if (t.isNotEmpty) {
      runs.add((text: t, bold: bold, italic: italic, strike: strike, code: false, spoiler: spoiler));
    }
  }

  var at = 0;
  for (final m in _inline.allMatches(text)) {
    plain(text.substring(at, m.start));
    if (m.group(1) != null) {
      final code = m.group(1)!;
      runs.add((
        text: code.substring(1, code.length - 1),
        bold: bold,
        italic: italic,
        strike: strike,
        code: true,
        spoiler: spoiler
      ));
    } else if (m.group(2) != null) {
      runs.addAll(styleRuns(m.group(3)!, bold: true, italic: italic, strike: strike, spoiler: spoiler));
    } else if (m.group(4) != null) {
      runs.addAll(styleRuns(m.group(5)!, bold: bold, italic: italic, strike: true, spoiler: spoiler));
    } else if (m.group(6) != null) {
      runs.addAll(styleRuns(m.group(7)!, bold: bold, italic: italic, strike: strike, spoiler: true));
    } else if (m.group(8) != null) {
      runs.addAll(styleRuns(m.group(9)!, bold: bold, italic: true, strike: strike, spoiler: spoiler));
    } else {
      runs.addAll(styleRuns(m.group(11)!, bold: bold, italic: true, strike: strike, spoiler: spoiler));
    }
    at = m.end;
  }
  plain(text.substring(at));
  return runs;
}

/// A message cut into blocks: ordinary text, quoted lines, or a block of code.
typedef TextBlock = ({String kind, String text}); // kind: text, quote, code, list, h1, h2 or h3

List<TextBlock> splitBlocks(String text) {
  final blocks = <TextBlock>[];
  final lines = text.split('\n');
  final pending = <String>[];
  var kind = 'text';
  void flush() {
    if (pending.isNotEmpty) blocks.add((kind: kind, text: pending.join('\n')));
    pending.clear();
  }

  var inCode = false;
  for (final line in lines) {
    if (line.trimLeft().startsWith('```')) {
      // A fence opens or closes a block of code; text on the same line after an opening fence is kept.
      flush();
      inCode = !inCode;
      kind = inCode ? 'code' : 'text';
      final rest = line.trimLeft().substring(3);
      if (inCode && rest.trim().isNotEmpty && rest.contains(' ')) pending.add(rest);
      continue;
    }
    if (inCode) {
      pending.add(line);
      continue;
    }
    // "# ", "## " and "### " start a heading: a block of its own.
    final heading = RegExp(r'^(#{1,3}) +(\S.*)$').firstMatch(line);
    if (heading != null) {
      flush();
      blocks.add((kind: 'h${heading.group(1)!.length}', text: heading.group(2)!.trimRight()));
      kind = 'text';
      continue;
    }
    final quoted = line.startsWith('> ') || line == '>';
    final bullet = RegExp(r'^\s*[-*] +\S').hasMatch(line);
    final next = quoted
        ? 'quote'
        : bullet
            ? 'list'
            : 'text';
    if (next != kind) {
      flush();
      kind = next;
    }
    pending.add(quoted
        ? line.substring(line.length > 1 ? 2 : 1)
        : bullet
            ? line.replaceFirst(RegExp(r'^\s*[-*] +'), '')
            : line);
  }
  flush();
  return blocks;
}

/// Message text: styled, with web addresses, mentions and #channels that can
/// be tapped.
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
  bool _spoilersShown = false;

  @override
  void dispose() {
    for (final r in _recognizers) {
      r.dispose();
    }
    super.dispose();
  }

  TapGestureRecognizer _tap(VoidCallback onTap) {
    final r = TapGestureRecognizer()..onTap = onTap;
    _recognizers.add(r);
    return r;
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

  // Plain words with each "@name" and "#channel" picked out.
  List<InlineSpan> _marked(String text, TextStyle? style) {
    final spans = <InlineSpan>[];
    var at = 0;
    final marks = <(int, int, String?)>[
      for (final (start, end) in mentionSpans(text)) (start, end, null),
      for (final m in _channelWord.allMatches(text))
        if (widget.channels.containsKey(m.group(2)!.toLowerCase()))
          (m.start + m.group(1)!.length, m.end, widget.channels[m.group(2)!.toLowerCase()]),
    ]..sort((a, b) => a.$1.compareTo(b.$1));
    for (final (start, end, roomId) in marks) {
      if (start < at) continue;
      if (start > at) spans.add(TextSpan(text: text.substring(at, start), style: style));
      spans.add(TextSpan(
          text: text.substring(start, end),
          recognizer:
              roomId != null && widget.onChannel != null ? _tap(() => widget.onChannel!(roomId)) : null,
          style: (style ?? const TextStyle()).copyWith(
              color: widget.linkColor,
              fontWeight: FontWeight.w600,
              decoration: roomId != null ? TextDecoration.underline : null,
              decorationColor: widget.linkColor)));
      at = end;
    }
    if (at < text.length) spans.add(TextSpan(text: text.substring(at), style: style));
    return spans;
  }

  // One stretch of ordinary text: links first, then styling, then mentions.
  List<InlineSpan> _spans(String text) {
    final spans = <InlineSpan>[];
    final base = widget.style?.color ?? DefaultTextStyle.of(context).style.color ?? Colors.black;
    for (final piece in splitLinks(text)) {
      if (piece.link != null) {
        spans.add(TextSpan(
            text: piece.text,
            recognizer: _tap(() => _open(piece.link!)),
            style: TextStyle(
                color: widget.linkColor,
                decoration: TextDecoration.underline,
                decorationColor: widget.linkColor)));
        continue;
      }
      for (final run in styleRuns(piece.text)) {
        var style = TextStyle(
          fontWeight: run.bold ? FontWeight.bold : null,
          fontStyle: run.italic ? FontStyle.italic : null,
          decoration: run.strike ? TextDecoration.lineThrough : null,
        );
        if (run.code) {
          style = style.copyWith(fontFamily: 'monospace', backgroundColor: base.withValues(alpha: 0.12));
        }
        if (run.spoiler && !_spoilersShown) {
          // Covered until tapped: the words are drawn in the colour of their cover.
          spans.add(TextSpan(
              text: run.text,
              recognizer: _tap(() => setState(() => _spoilersShown = true)),
              style:
                  style.copyWith(color: Colors.transparent, backgroundColor: base.withValues(alpha: 0.85))));
        } else if (run.code) {
          spans.add(TextSpan(text: run.text, style: style));
        } else {
          spans.addAll(_marked(run.text, style));
        }
      }
    }
    return spans;
  }

  @override
  Widget build(BuildContext context) {
    for (final r in _recognizers) {
      r.dispose();
    }
    _recognizers.clear();
    final blocks = splitBlocks(widget.text);
    final base = widget.style?.color ?? DefaultTextStyle.of(context).style.color ?? Colors.black;
    Widget block(TextBlock b) {
      switch (b.kind) {
        case 'code':
          return Container(
            width: double.infinity,
            margin: const EdgeInsets.symmetric(vertical: 3),
            padding: const EdgeInsets.all(8),
            decoration:
                BoxDecoration(color: base.withValues(alpha: 0.10), borderRadius: BorderRadius.circular(8)),
            child: SelectableText(b.text,
                style: (widget.style ?? const TextStyle()).copyWith(fontFamily: 'monospace', fontSize: 13)),
          );
        case 'quote':
          return Container(
            margin: const EdgeInsets.symmetric(vertical: 2),
            padding: const EdgeInsets.only(left: 8),
            decoration:
                BoxDecoration(border: Border(left: BorderSide(color: base.withValues(alpha: 0.4), width: 3))),
            child: Text.rich(TextSpan(
                style: widget.style?.copyWith(color: base.withValues(alpha: 0.8)), children: _spans(b.text))),
          );
        case 'h1' || 'h2' || 'h3':
          final scale = b.kind == 'h1'
              ? 1.5
              : b.kind == 'h2'
                  ? 1.3
                  : 1.15;
          final size = (widget.style?.fontSize ?? DefaultTextStyle.of(context).style.fontSize ?? 16) * scale;
          return Padding(
            padding: const EdgeInsets.only(top: 2, bottom: 2),
            child: Text.rich(TextSpan(
                style:
                    (widget.style ?? const TextStyle()).copyWith(fontSize: size, fontWeight: FontWeight.bold),
                children: _spans(b.text))),
          );
        case 'list':
          return Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                for (final item in b.text.split('\n'))
                  Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Text('  •  ', style: widget.style),
                    Flexible(child: Text.rich(TextSpan(style: widget.style, children: _spans(item)))),
                  ]),
              ]);
        default:
          return Text.rich(TextSpan(style: widget.style, children: _spans(b.text)));
      }
    }

    if (blocks.isEmpty) return Text(widget.text, style: widget.style);
    if (blocks.length == 1) return block(blocks.single);
    return Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: [for (final b in blocks) block(b)]);
  }
}
