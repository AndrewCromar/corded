import 'package:corded_app/app_state.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a reaction bar is read out of free text', () {
    expect(AppState.parseReactions('👍 ❤️  😂'), ['👍', '❤️', '😂']);
    expect(AppState.parseReactions('👍👍🎉'), ['👍', '🎉']); // no repeats
    expect(AppState.parseReactions('   '), isEmpty);
  });

  test('emoji made of several code points stay whole', () {
    expect(AppState.parseReactions('👨‍👩‍👧🏳️‍🌈👍🏽'), ['👨‍👩‍👧', '🏳️‍🌈', '👍🏽']);
  });

  test('at most eight are kept', () {
    expect(AppState.parseReactions('1234567890').length, 8);
  });
}
