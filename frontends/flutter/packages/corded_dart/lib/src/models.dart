// Plain records of what the core reports. Field names follow the JSON.

class Member {
  Member.fromJson(Map<String, dynamic> j)
      : userId = j['user_id'] as String? ?? '',
        username = j['username'] as String? ?? '',
        displayName = j['display_name'] as String? ?? j['username'] as String? ?? '',
        me = j['me'] == true,
        isOwner = j['is_owner'] == true,
        online = j['online'] == true,
        roles = ((j['roles'] as List?) ?? const []).map((r) => '$r').toList();
  final String userId, username, displayName;
  final bool me, isOwner, online;
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
  int lastActivity = 0;

  void apply(Map<String, dynamic> j) {
    serverId = (j['server_id'] as num?)?.toInt() ?? serverId;
    title = j['title'] as String? ?? title;
    kind = j['kind'] as String? ?? kind;
    unread = (j['unread'] as num?)?.toInt() ?? unread;
    disappearAfter = (j['disappear_after'] as num?)?.toInt() ?? disappearAfter;
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

  void apply(Map<String, dynamic> j) {
    status = j['status'] as String? ?? status;
    edited = j['edited'] == true;
    final content = (j['content'] as Map?) ?? const {};
    if (content['body'] is String) body = content['body'] as String;
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

class ServerInfo {
  ServerInfo(this.id);
  final int id;
  String name = '';
  String address = '';
  String connection = 'disconnected';
  bool isOwner = false;
  Set<String> permissions = {};

  void apply(Map<String, dynamic> j) {
    name = j['name'] as String? ?? name;
    address = j['address'] as String? ?? address;
    connection = j['connection'] as String? ?? connection;
    isOwner = j['is_owner'] as bool? ?? isOwner;
    final p = j['my_permissions'] as List?;
    if (p != null) permissions = p.map((e) => '$e').toSet();
  }
}
