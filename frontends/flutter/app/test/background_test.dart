import 'package:corded_app/background.dart';
import 'package:flutter_test/flutter_test.dart';

Map<String, dynamic> message(String body, {bool mine = false, String type = 'm.text', bool shared = false}) =>
    {
      'event': 'event_received',
      'data': {
        'room_id': 'r1',
        'type': type,
        'mine': mine,
        'shared_history': shared,
        'sender_name': 'bob',
        'content': {'body': body},
      },
    };

void main() {
  test('a message from someone else in a channel is announced with who said it', () {
    final n = notificationFor(message('hello'), {'r1': '#general'})!;
    expect(n.title, '#general');
    expect(n.body, 'bob: hello');
  });

  test('a tap leads to the chat, and to the thread if the message was in one', () {
    expect(notificationFor(message('hello'), {})!.payload, '{"room_id":"r1"}');
    final threaded = message('hello');
    (threaded['data'] as Map)['relation'] = {'kind': 'thread', 'target': 'root1'};
    expect(notificationFor(threaded, {})!.payload, '{"room_id":"r1","thread":"root1"}');
  });

  test('a muted chat is silent, and hidden text says who but not what', () {
    expect(notificationFor(message('hello'), {'r1': '#general'}, muted: {'r1'}), isNull);
    final hidden = notificationFor(message('a secret'), {'r1': '#general'}, showText: false)!;
    expect(hidden.title, '#general');
    expect(hidden.body, 'New message from bob');
    expect(notificationFor(message('a secret'), {'r1': 'bob'}, showText: false)!.body, 'New message');
  });

  test('a mention says so, and gets through a muted chat', () {
    final n = notificationFor(message('@andrew lunch?'), {'r1': '#general'}, muted: {'r1'}, me: 'andrew')!;
    expect(n.title, '#general · mentioned you');
    expect(n.body, 'bob: @andrew lunch?');
    expect(notificationFor(message('@carol lunch?'), {'r1': '#general'}, muted: {'r1'}, me: 'andrew'), isNull);
    final hidden = notificationFor(message('@everyone hi'), {'r1': '#general'}, showText: false, me: 'andrew')!;
    expect(hidden.body, 'bob mentioned you');
  });

  test('in a direct chat the title is the sender and the body is just the text', () {
    final n = notificationFor(message('hello'), {'r1': 'bob'})!;
    expect(n.title, 'bob');
    expect(n.body, 'hello');
  });

  test('nothing is announced for your own messages, reactions, old history or other events', () {
    expect(notificationFor(message('mine', mine: true), {}), isNull);
    expect(notificationFor(message('+1', type: 'm.reaction'), {}), isNull);
    expect(notificationFor(message('old', shared: true), {}), isNull);
    expect(notificationFor({'event': 'typing', 'room_id': 'r1'}, {}), isNull);
  });
}
