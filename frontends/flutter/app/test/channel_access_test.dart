import 'package:corded_app/screens/channel_access.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('each choice becomes an exception and reads back as the same choice', () {
    for (final choice in ChannelAccess.values) {
      final rule = exceptionFor(choice);
      expect(accessFrom(rule.allow.toSet(), rule.deny.toSet()), choice);
    }
  });

  test('rules made in the terminal client are understood', () {
    // /channel private: everyone loses sight of it, a role gets it back.
    expect(accessFrom({}, {'view_channel'}), ChannelAccess.none);
    expect(accessFrom({'view_channel'}, {}), ChannelAccess.write);
    // read-only: writing is taken away.
    expect(accessFrom({}, {'send_messages'}), ChannelAccess.read);
    expect(accessFrom({}, {}), ChannelAccess.same);
  });
}
