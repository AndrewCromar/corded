// Plain records of what the core reports. Field names follow the JSON.

class Member {
  Member.fromJson(Map<String, dynamic> j)
      : userId = j['user_id'] as String? ?? '',
        username = j['username'] as String? ?? '',
        displayName = j['display_name'] as String? ?? j['username'] as String? ?? '',
        me = j['me'] == true,
        isOwner = j['is_owner'] == true,
        isAdmin = j['is_admin'] == true,
        nickname = j['nickname'] as String? ?? '',
        online = j['online'] == true,
        roles = ((j['roles'] as List?) ?? const []).map((r) => '$r').toList();
  final String userId, username, displayName, nickname;
  final bool me, isOwner, isAdmin, online;
  final List<String> roles;
}

class Room {
  Room(this.id);
  final String id;
  int serverId = 0;
  String title = '';
  String kind = 'direct'; // channel, direct or group
  int unread = 0;
  int disappearAfter = 0;
  List<Member> members = const [];
  List<String> pinned = const []; // ids of the pinned messages, oldest pin first
  int lastActivity = 0;

  void apply(Map<String, dynamic> j) {
    serverId = (j['server_id'] as num?)?.toInt() ?? serverId;
    title = j['title'] as String? ?? title;
    kind = j['kind'] as String? ?? kind;
    unread = (j['unread'] as num?)?.toInt() ?? unread;
    disappearAfter = (j['disappear_after'] as num?)?.toInt() ?? disappearAfter;
    final p = j['pinned'] as List?;
    if (p != null) pinned = p.map((e) => '$e').toList();
    final m = j['members'] as List?;
    if (m != null) members = m.map((e) => Member.fromJson((e as Map).cast<String, dynamic>())).toList();
  }
}

class Message {
  Message.fromJson(Map<String, dynamic> j)
      : id = j['event_id'] as String? ?? '',
        roomId = j['room_id'] as String? ?? '',
        type = j['type'] as String? ?? '',
        sender = j['sender_name'] as String? ?? '?',
        mine = j['mine'] == true,
        timestamp = (j['origin_ts'] as num?)?.toInt() ?? 0 {
    apply(j);
  }
  final String id, roomId, type, sender;
  final bool mine;
  final int timestamp;
  String body = '';
  String status = 'ok'; // ok, pending, failed, undecryptable, redacted
  bool edited = false;
  String? replyTo;
  String? threadRoot;

  /// Where the server placed this message in the room; the same for everyone.
  /// Null until the server has confirmed a message this device sent.
  int? seq;

  /// Whether it mentions the person using this device (set by the store).
  bool mentionsMe = false;

  /// How many thread replies the core knew of when it reported this message.
  int threadCount = 0;

  void apply(Map<String, dynamic> j) {
    status = j['status'] as String? ?? status;
    edited = j['edited'] == true;
    seq = (j['seq'] as num?)?.toInt() ?? seq;
    threadCount = (j['thread_count'] as num?)?.toInt() ?? threadCount;
    final content = (j['content'] as Map?) ?? const {};
    if (content['body'] is String) body = content['body'] as String;
    // A reply made inside a thread names what it quotes here.
    if (content['reply_to'] is String) replyTo = content['reply_to'] as String;
    final relation = j['relation'] as Map?;
    if (relation != null) {
      if (relation['kind'] == 'reply') replyTo = relation['target'] as String?;
      if (relation['kind'] == 'thread') threadRoot = relation['target'] as String?;
    }
  }

  /// What to draw for this message.
  String get text => switch (status) {
        'redacted' => 'This message was deleted',
        'undecryptable' => 'This message could not be decrypted',
        _ => body,
      };
}

/// A role on a server and what it lets its holders do.
class Role {
  Role.fromJson(Map<String, dynamic> j)
      : name = '${j['name']}',
        isEveryone = j['is_everyone'] == true,
        position = (j['position'] as num?)?.toInt() ?? 0,
        permissions = ((j['permissions'] as List?) ?? const []).map((e) => '$e').toSet();
  final String name;
  final bool isEveryone; // the role every member has
  final int position;
  final Set<String> permissions;
}

class ServerInfo {
  ServerInfo(this.id);
  final int id;
  String name = '';
  String address = '';
  String connection = 'disconnected';
  bool isOwner = false;
  Set<String> permissions = {};
  List<String> roles = const []; // the roles that can be given to members
  List<Role> roleDetails = const []; // every role, the one everybody has included

  /// Whether this person may do something that needs [permission] here.
  bool can(String permission) =>
      isOwner || permissions.contains('administrator') || permissions.contains(permission);

  void apply(Map<String, dynamic> j) {
    name = j['name'] as String? ?? name;
    address = j['address'] as String? ?? address;
    connection = j['connection'] as String? ?? connection;
    isOwner = j['is_owner'] as bool? ?? isOwner;
    final p = j['my_permissions'] as List?;
    if (p != null) permissions = p.map((e) => '$e').toSet();
    final r = j['roles'] as List?;
    if (r != null) {
      roleDetails = [
        for (final role in r)
          if (role is Map) Role.fromJson(role.cast<String, dynamic>())
      ]..sort((a, b) => b.position.compareTo(a.position));
      roles = [
        for (final role in r)
          if (role is Map && role['is_everyone'] != true) '${role['name']}'
      ];
    }
  }
}
