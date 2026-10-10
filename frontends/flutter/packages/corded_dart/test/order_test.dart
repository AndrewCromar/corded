import 'package:corded_dart/corded_dart.dart';
import 'package:test/test.dart';

Message msg(String id, {int? seq, required int ts}) => Message.fromJson({
      'event_id': id,
      'room_id': 'r',
      'type': 'm.text',
      'origin_ts': ts,
      'seq': seq,
      'content': {'body': id},
    });

void main() {
  test('messages are ordered by the server, not by the senders\' clocks', () {
    // The phone's clock runs 80 seconds ahead of the PC's. The PC replied
    // after the phone's message, yet its timestamp is earlier.
    final phone = msg('from the phone', seq: 10, ts: 1000080);
    final reply = msg('reply from the pc', seq: 11, ts: 1000005);
    final pending = msg('still sending', ts: 900000);
    final older = msg('older', seq: 3, ts: 2000000);
    final list = [pending, reply, phone, older]..sort(CordedStore.compareMessages);
    expect(list.map((m) => m.id), ['older', 'from the phone', 'reply from the pc', 'still sending']);
  });

  test('messages still being sent keep the order they were written in', () {
    final list = [msg('second', ts: 2), msg('first', ts: 1)]..sort(CordedStore.compareMessages);
    expect(list.map((m) => m.id), ['first', 'second']);
  });
}
