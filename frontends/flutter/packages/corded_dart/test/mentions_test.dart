import 'package:corded_dart/corded_dart.dart';
import 'package:test/test.dart';

void main() {
  test('a name after @ is a mention, whatever its case', () {
    expect(mentionsUser('hey @andrew look', 'andrew'), isTrue);
    expect(mentionsUser('@Andrew: look', 'andrew'), isTrue);
    expect(mentionsUser('look, @andrew.', 'andrew'), isTrue);
    expect(mentionsUser('(@android-phone)', 'android-phone'), isTrue);
  });

  test('part of a longer name, or an email address, is not', () {
    expect(mentionsUser('hey @andrewcromar', 'andrew'), isFalse);
    expect(mentionsUser('mail me at scott@andrew.org', 'andrew'), isFalse);
    expect(mentionsUser('andrew without the sign', 'andrew'), isFalse);
  });

  test('@everyone mentions everybody', () {
    expect(mentionsUser('@everyone lunch?', 'bob'), isTrue);
    expect(mentionedNames('@a and @B and @everyone'), {'a', 'b', 'everyone'});
  });

  test('where the mentions are', () {
    expect(mentionSpans('hi @bob and @al-1!'), [(3, 7), (12, 17)]);
  });
}
