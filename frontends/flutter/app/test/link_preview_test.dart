import 'package:corded_app/link_preview.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('the first link in a message is found, without the sentence around it', () {
    expect(firstLink('look at https://example.com/a?b=1, it is good'), 'https://example.com/a?b=1');
    expect(firstLink('(see http://example.org/x).'), 'http://example.org/x');
    expect(firstLink('no link here'), isNull);
    expect(firstLink('ftp://example.com'), isNull);
  });

  test('a page describes itself in meta tags, with the title tag as a fallback', () {
    final page = readPage('''
<html><head><title>Plain &amp; simple</title>
<meta property="og:title" content="A &quot;fine&quot; page">
<meta content="What it is about" name="description">
<meta property='og:image' content='/pic.png'>
</head></html>''');
    expect(page.title, 'A "fine" page');
    expect(page.description, 'What it is about');
    expect(page.image, '/pic.png');
    expect(readPage('<title>Only a title</title>').title, 'Only a title');
    expect(readPage('nothing').title, '');
  });

  test('a card survives the trip inside a message, and junk is refused', () {
    const card = LinkPreview(url: 'https://example.com', title: 'T', description: 'D');
    final back = LinkPreview.fromJson(card.toJson())!;
    expect((back.url, back.title, back.description), ('https://example.com', 'T', 'D'));
    expect(LinkPreview.fromJson({'url': 'javascript:alert(1)', 'title': 'x'}), isNull);
    expect(LinkPreview.fromJson({'url': 'https://example.com'}), isNull);
    expect(LinkPreview.fromJson('nope'), isNull);
    // Overlong text from someone else's client is cut.
    expect(LinkPreview.fromJson({'url': 'https://e.com', 'title': 'x' * 500})!.title.length, 140);
  });
}
