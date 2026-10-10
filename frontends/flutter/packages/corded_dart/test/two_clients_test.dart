// Two cores, driven from Dart, exchange a message through a real server.
//
// Needs the native build:  CORDED_BIN=<dir with cordedd>  CORDED_LIB=<path to libcorded.so>
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:corded_dart/corded_dart.dart';
import 'package:test/test.dart';

Future<T> eventually<T>(T? Function() probe, String what, {Duration timeout = const Duration(seconds: 20)}) async {
  final deadline = DateTime.now().add(timeout);
  while (DateTime.now().isBefore(deadline)) {
    final v = probe();
    if (v != null) return v;
    await Future<void>.delayed(const Duration(milliseconds: 50));
  }
  fail('timed out waiting for: $what');
}

void main() {
  final bin = Platform.environment['CORDED_BIN'];
  final lib = Platform.environment['CORDED_LIB'];
  final skip = bin == null || lib == null ? 'set CORDED_BIN and CORDED_LIB to run' : null;

  test('two clients exchange messages, with unread counts, receipts and paging', () async {
    final tmp = await Directory.systemTemp.createTemp('corded_dart_test');
    final server = await Process.start(
        '$bin/cordedd', ['--port', '0', '--data', '${tmp.path}/server', '--owner', 'alice']);
    addTearDown(() async {
      server.kill();
      await server.exitCode;
      await tmp.delete(recursive: true);
    });
    // The server prints the port it was given.
    final port = Completer<int>();
    final lines = <String>[];
    for (final stream in [server.stdout, server.stderr]) {
      stream.transform(utf8.decoder).transform(const LineSplitter()).listen((line) {
        lines.add(line);
        final m = RegExp(r'listening on .*:(\d+)').firstMatch(line);
        if (m != null && !port.isCompleted) port.complete(int.parse(m.group(1)!));
      });
    }
    final p = await port.future.timeout(const Duration(seconds: 15),
        onTimeout: () => fail('the server did not start:\n${lines.join('\n')}'));

    Future<(CordedEngine, CordedStore)> client(String name) async {
      final engine = await CordedEngine.open('${tmp.path}/$name', fastKdf: true, libraryPath: lib);
      final store = CordedStore(engine);
      addTearDown(() async {
        await store.dispose();
        await engine.close();
      });
      expect(engine.vaultExists(), isFalse);
      await engine.createVault('a long passphrase', name);
      await engine.command({'cmd': 'connect', 'host': '127.0.0.1', 'port': p});
      return (engine, store);
    }

    final (alice, aliceStore) = await client('alice');
    final (bob, bobStore) = await client('bob');
    expect(alice.apiVersion, greaterThanOrEqualTo(2));

    Room? general(CordedStore s) => s.rooms.values.where((r) => r.title == '#general').firstOrNull;
    final room = await eventually(() => general(bobStore), "bob's #general");
    await eventually(() => (general(aliceStore)?.members.length ?? 0) == 2 ? true : null, 'alice sees bob');
    expect(aliceStore.servers.values.single.isOwner, isTrue);

    // A refused command surfaces as an error, not silence.
    await expectLater(bob.command({'cmd': 'create_channel', 'name': 'nope'}), throwsA(isA<CordedError>()));

    // Each sees the other online; do not disturb and invisible show through.
    final bobId = general(aliceStore)!.members.firstWhere((m) => m.username == 'bob').userId;
    final serverId = aliceStore.servers.values.single.id;
    await eventually(() => aliceStore.presence(serverId, bobId) == 'online' ? true : null, 'bob online');
    await bob.command({'cmd': 'set_presence', 'status': 'dnd'});
    await eventually(() => aliceStore.presence(serverId, bobId) == 'dnd' ? true : null, 'bob dnd');
    await bob.command({'cmd': 'set_presence', 'status': 'invisible'});
    await eventually(() => aliceStore.presence(serverId, bobId) == 'offline' ? true : null, 'bob invisible');
    await bob.command({'cmd': 'set_presence', 'status': 'auto'});
    await eventually(() => aliceStore.presence(serverId, bobId) == 'online' ? true : null, 'bob back');

    // Who may manage whom, and the roles there are to give.
    final aliceServer = aliceStore.servers.values.single, bobServer = bobStore.servers.values.single;
    expect(aliceServer.can('kick_members'), isTrue);
    expect(bobServer.can('kick_members'), isFalse);
    await alice.command({'cmd': 'create_role', 'name': 'helper', 'permissions': ['kick_members']});
    await eventually(() => aliceServer.roles.contains('helper') ? true : null, 'the new role');
    // A role's permissions can be changed without touching its name.
    Role helper() => aliceServer.roleDetails.firstWhere((r) => r.name == 'helper');
    expect(helper().permissions, {'kick_members'});
    expect(aliceServer.roleDetails.any((r) => r.isEveryone), isTrue);
    await alice.command({
      'cmd': 'edit_role',
      'role': 'helper',
      'name': 'helper',
      'permissions': ['kick_members', 'manage_nicknames'],
    });
    await eventually(() => helper().permissions.contains('manage_nicknames') ? true : null, 'the edited role');
    expect(helper().name, 'helper');
    await alice.command({'cmd': 'grant_role', 'username': 'bob', 'role': 'helper', 'grant': true});
    await eventually(() => bobServer.can('kick_members') ? true : null, 'bob may kick now');
    final listed = await alice.command({'cmd': 'member_list'});
    final members = [for (final m in listed['members'] as List) Member.fromJson((m as Map).cast<String, dynamic>())];
    expect(members.firstWhere((m) => m.username == 'bob').roles, ['helper']);
    expect(members.firstWhere((m) => m.username == 'alice').isOwner, isTrue);
    await alice.command({'cmd': 'set_nickname', 'username': 'bob', 'nickname': 'Bobby'});
    await eventually(
        () => general(aliceStore)!.members.any((m) => m.displayName == 'Bobby') ? true : null, 'the display name');
    await alice.command({'cmd': 'set_nickname', 'username': 'bob', 'nickname': ''});
    await eventually(
        () => general(aliceStore)!.members.any((m) => m.displayName == 'bob') ? true : null, 'the name restored');
    await eventually(
        () => general(bobStore)!.members.any((m) => m.me && m.displayName == 'bob') ? true : null, 'restored for bob');

    for (var i = 1; i <= 5; i++) {
      await alice.command({'cmd': 'send_text', 'room_id': room.id, 'body': 'message $i'});
    }
    await eventually(() => bobStore.messages(room.id).length == 5 ? true : null, 'five messages at bob');
    expect(bobStore.messages(room.id).map((m) => m.body), [for (var i = 1; i <= 5; i++) 'message $i']);
    expect(bobStore.messages(room.id).first.sender, 'alice');
    expect(bobStore.rooms[room.id]!.unread, 5);

    // Opening the chat reads it: the count drops, and alice learns of it.
    await bobStore.open(room.id);
    await eventually(() => bobStore.rooms[room.id]!.unread == 0 ? true : null, 'unread cleared');
    final last = aliceStore.messages(room.id).last;
    await eventually(() => aliceStore.readBy(room.id, last.id).contains('bob') ? true : null, 'read by bob');
    await eventually(() => last.status == 'ok' ? true : null, 'sent confirmation');

    // Typing shows up and lapses on its own.
    await bob.command({'cmd': 'typing', 'room_id': room.id});
    await eventually(() => aliceStore.typing(room.id).contains('bob') ? true : null, 'bob typing');

    // A reply, a reaction and an edit arrive as what they are.
    await bob.command({'cmd': 'send_text', 'room_id': room.id, 'body': 'a reply', 'reply_to': last.id});
    final reply = await eventually(
        () => aliceStore.messages(room.id).where((m) => m.body == 'a reply').firstOrNull, 'the reply');
    expect(reply.replyTo, last.id);
    expect(aliceStore.typing(room.id), isEmpty); // sending ends the notice
    await bob.command({
      'cmd': 'send_event',
      'room_id': room.id,
      'type': 'm.reaction',
      'content': {'key': '+1'},
      'relation': {'kind': 'annotation', 'target': last.id, 'key': '+1'},
    });
    await eventually(() => aliceStore.reactions(room.id, last.id)['+1'] == 1 ? true : null, 'the reaction');
    // Reacting twice with the same emoji takes the reaction back.
    expect(aliceStore.myReaction(room.id, last.id, '+1'), isNull); // it is bob's, not alice's
    await aliceStore.toggleReaction(room.id, last.id, 'ok');
    await eventually(() => bobStore.reactions(room.id, last.id)['ok'] == 1 ? true : null, "alice's reaction");
    await eventually(() => aliceStore.myReaction(room.id, last.id, 'ok'), 'her own reaction known');
    await aliceStore.toggleReaction(room.id, last.id, 'ok');
    await eventually(() => bobStore.reactions(room.id, last.id)['ok'] == null ? true : null, 'reaction taken back');
    await eventually(() => aliceStore.myReaction(room.id, last.id, 'ok') == null ? true : null, 'gone for alice too');
    expect(bobStore.reactions(room.id, last.id)['+1'], 1);

    await alice.command({'cmd': 'edit_event', 'room_id': room.id, 'event_id': last.id, 'body': 'message 5, edited'});
    await eventually(
        () => bobStore.messages(room.id).any((m) => m.body == 'message 5, edited' && m.edited) ? true : null,
        'the edit');

    // A thread reply stays out of the conversation and is counted on its first message.
    final before = aliceStore.messages(room.id).length;
    await bob.command({'cmd': 'send_text', 'room_id': room.id, 'body': 'in the thread', 'thread': last.id});
    await eventually(() => aliceStore.thread(room.id, last.id).length == 1 ? true : null, 'the thread reply');
    expect(aliceStore.messages(room.id).length, before);
    expect(aliceStore.threadCount(room.id, last), 1);
    expect(bobStore.thread(room.id, last.id).single.body, 'in the thread');

    // Inside a thread a message can still quote another one.
    await alice.command({
      'cmd': 'send_text',
      'room_id': room.id,
      'body': 'quoting inside the thread',
      'thread': last.id,
      'reply_to': aliceStore.thread(room.id, last.id).single.id,
    });
    final quoting = await eventually(
        () => bobStore.thread(room.id, last.id).where((m) => m.body == 'quoting inside the thread').firstOrNull,
        'the quoting reply');
    expect(quoting.threadRoot, last.id);
    expect(quoting.replyTo, bobStore.thread(room.id, last.id).first.id);

    // A reply in a thread can start a thread of its own, to any depth.
    final inner = aliceStore.thread(room.id, last.id).first;
    await alice.command({'cmd': 'send_text', 'room_id': room.id, 'body': 'one level deeper', 'thread': inner.id});
    await eventually(() => bobStore.thread(room.id, inner.id).length == 1 ? true : null, 'the nested reply');
    expect(bobStore.thread(room.id, last.id).length, 2); // the outer thread is unchanged
    expect(bobStore.messages(room.id).length, before); // and so is the conversation
    expect(bobStore.threadCount(room.id, bobStore.thread(room.id, last.id).first), 1);
    final deepest = bobStore.thread(room.id, inner.id).single;
    await bob.command({'cmd': 'send_text', 'room_id': room.id, 'body': 'and deeper still', 'thread': deepest.id});
    await eventually(() => aliceStore.thread(room.id, deepest.id).length == 1 ? true : null, 'three levels');

    // Both sides hold the conversation in the server's order.
    for (final s in [aliceStore, bobStore]) {
      await eventually(() => s.messages(room.id).every((m) => m.seq != null) ? true : null, 'all confirmed');
      final seqs = s.messages(room.id).map((m) => m.seq!).toList();
      expect(seqs, [...seqs]..sort());
    }

    // The screen goes away but the process lives on: something else reads the
    // events, and a rebuilt screen gets the same core back, still connected.
    await bob.stopPump();
    expect(bob.pumping, isFalse);
    await alice.command({'cmd': 'send_text', 'room_id': room.id, 'body': 'while the screen was gone'});
    final drained = <String>[];
    await eventually(() {
      drained.addAll(CordedEngine.drain(bob.address, libraryPath: lib));
      return drained.any((e) => e.contains('while the screen was gone')) ? true : null;
    }, 'the event read from outside');
    final rebuilt = await CordedEngine.open('${tmp.path}/bob', fastKdf: true, libraryPath: lib, pump: false);
    expect(rebuilt.address, bob.address);
    final rebuiltStore = CordedStore(rebuilt);
    final replies = rebuilt.command({'cmd': 'status'});
    Map<String, dynamic>? status;
    replies.then((s) => status = s);
    await eventually(() {
      for (final e in CordedEngine.drain(rebuilt.address, libraryPath: lib)) {
        rebuilt.deliver(e);
      }
      return status;
    }, 'the status answer');
    expect(status!['vault'], 'unlocked');
    expect(status!['username'], 'bob');
    await rebuiltStore.dispose();
    await bob.startPump();
    // A rebuilt screen starts from an empty store and reads the vault.
    final freshStore = CordedStore(bob);
    await freshStore.refresh();
    await freshStore.open(room.id);
    expect(freshStore.messages(room.id).any((m) => m.body == 'while the screen was gone'), isTrue);
    await freshStore.dispose();
    await bobStore.open(room.id);

    // The owner pins a message; everyone's copy of the room says so.
    await alice.command({'cmd': 'pin_event', 'room_id': room.id, 'event_id': last.id});
    await eventually(() => bobStore.rooms[room.id]!.pinned.contains(last.id) ? true : null, 'the pin');
    await expectLater(
        bob.command({'cmd': 'pin_event', 'room_id': room.id, 'event_id': last.id, 'pinned': false}),
        throwsA(isA<CordedError>()));
    await alice.command({'cmd': 'pin_event', 'room_id': room.id, 'event_id': last.id, 'pinned': false});
    await eventually(() => bobStore.rooms[room.id]!.pinned.isEmpty ? true : null, 'unpinned');

    // Leaving a server takes it and its chats off this device only.
    await alice.command({'cmd': 'forget_server', 'server_id': aliceServer.id});
    await eventually(() => aliceStore.servers.isEmpty && aliceStore.rooms.isEmpty ? true : null, 'the server gone');
    expect((await alice.command({'cmd': 'list_servers'}))['servers'], isEmpty);
    expect(general(bobStore)!.members.length, 2); // her account is still on the server

    // A fresh start reads everything back from the vault.
    await bobStore.dispose();
    await bob.close();
    final again = await CordedEngine.open('${tmp.path}/bob', fastKdf: true, libraryPath: lib);
    final againStore = CordedStore(again);
    addTearDown(() async {
      await againStore.dispose();
      await again.close();
    });
    expect(again.vaultExists(), isTrue);
    await expectLater(again.unlock('wrong'), throwsA(isA<CordedError>()));
    await again.unlock('a long passphrase');
    // With the vault open, unlock only checks a passphrase.
    expect((await again.unlock('a long passphrase'))['already_open'], isTrue);
    await expectLater(again.unlock('not it'), throwsA(isA<CordedError>()));
    await eventually(() => againStore.rooms[room.id], 'rooms after restart');
    await againStore.open(room.id);
    expect(againStore.messages(room.id).length, 7);
    await againStore.openThread(room.id, last.id);
    expect(againStore.thread(room.id, last.id).length, 2);
    expect(againStore.username, 'bob');
  }, skip: skip, timeout: const Timeout(Duration(minutes: 2)));
}
