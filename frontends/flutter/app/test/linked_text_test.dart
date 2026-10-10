import 'package:corded_app/screens/linked_text.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('addresses are found and sentence punctuation is left out', () {
    final p = splitLinks('see https://example.org/a?b=1, then http://x.io. ok');
    expect(p.map((e) => e.text), ['see ', 'https://example.org/a?b=1', ', then ', 'http://x.io', '. ok']);
    expect(p.where((e) => e.link != null).map((e) => e.link!.host), ['example.org', 'x.io']);
  });

  test('text without addresses is one plain piece', () {
    final p = splitLinks('no links here: corded://host is not a web address');
    expect(p.length, 1);
    expect(p.single.link, isNull);
  });

  test('a message that is only an address', () {
    final p = splitLinks('https://github.com/AndrewCromar/corded');
    expect(p.single.link!.path, '/AndrewCromar/corded');
  });
}
