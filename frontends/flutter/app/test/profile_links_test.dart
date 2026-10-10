import 'package:corded_app/screens/profile.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a link keeps its label and reads back', () {
    final kept = joinProfileLink('GitHub', 'https://github.com/AndrewCromar');
    expect(kept, 'GitHub|https://github.com/AndrewCromar');
    final parts = splitProfileLink(kept);
    expect(parts.label, 'GitHub');
    expect(parts.url, 'https://github.com/AndrewCromar');
  });

  test('a link without a label is just its address', () {
    expect(joinProfileLink('', 'https://example.org'), 'https://example.org');
    expect(splitProfileLink('https://example.org').label, '');
  });

  test('a bar in a label cannot break the link apart', () {
    expect(splitProfileLink(joinProfileLink('a|b', 'https://x.io')).url, 'https://x.io');
  });
}
