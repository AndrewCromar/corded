import 'dart:convert';
import 'dart:typed_data';

// Plain records of what the core reports. Field names follow the JSON.

class Member {
  Member.fromJson(Map<String, dynamic> j)
      : userId = j['user_id'] as String? ?? '',
        username = j['username'] as String? ?? '',
        displayName = j['display_name'] as String? ?? j['username'] as String? ?? '',
        me = j['me'] == true,
        isOwner = j['is_owner'] == true,
        isAdmin = j['is_admin'] == true,
        bot = j['bot'] == true,
        nickname = j['nickname'] as String? ?? '',
        online = j['online'] == true,
        roles = ((j['roles'] as List?) ?? const []).map((r) => '$r').toList();
  final String userId, username, displayName, nickname;
  final bool me, isOwner, isAdmin, online;

  /// Their profile says they are a program, not a person.
  final bool bot;
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
  bool nsfw = false; // a channel marked so that clients warn before showing it
  bool archived = false; // a channel kept to read, closed to writing
  String channelType = ''; // how a channel is laid out: '' for messages, 'tasks' for a task list
  bool get isTaskList => channelType == 'tasks';
  String firstUnread = ''; // where this person's unread messages start; empty if none
  int lastActivity = 0;

  void apply(Map<String, dynamic> j) {
    serverId = (j['server_id'] as num?)?.toInt() ?? serverId;
    title = j['title'] as String? ?? title;
    kind = j['kind'] as String? ?? kind;
    unread = (j['unread'] as num?)?.toInt() ?? unread;
    disappearAfter = (j['disappear_after'] as num?)?.toInt() ?? disappearAfter;
    nsfw = j['nsfw'] as bool? ?? nsfw;
    archived = j['archived'] as bool? ?? archived;
    channelType = j['channel_type'] as String? ?? channelType;
    firstUnread = j['first_unread'] as String? ?? firstUnread;
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
        senderId = j['sender_user'] as String? ?? '',
        mine = j['mine'] == true,
        timestamp = (j['origin_ts'] as num?)?.toInt() ?? 0 {
    apply(j);
  }
  final String id, roomId, type, sender, senderId;
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

  /// For a poll: what can be chosen, and whether more than one may be.
  List<String> pollOptions = const [];
  bool pollMultiple = false;
  bool get isPoll => type == 'm.poll';

  /// For a file: what it is called, its kind and size, and for a picture a
  /// small preview and its shape. The file itself is fetched when wanted.
  String fileName = '';
  String fileMime = '';
  int fileSize = 0;
  Uint8List? thumbnail;
  int width = 0, height = 0;
  bool get isFile => type == 'm.file';

  /// For a task: whether it is ticked, and by whom.
  bool taskDone = false;
  String taskDoneBy = '';
  bool get isTask => type == 'm.task';
  bool get isImage => isFile && fileMime.startsWith('image/');

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
    if (type == 'm.file') {
      fileName = content['name'] as String? ?? fileName;
      fileMime = content['mime'] as String? ?? fileMime;
      fileSize = (content['size'] as num?)?.toInt() ?? fileSize;
      width = (content['width'] as num?)?.toInt() ?? width;
      height = (content['height'] as num?)?.toInt() ?? height;
      final small = content['thumbnail'];
      if (small is String && small.isNotEmpty && thumbnail == null) {
        try {
          thumbnail = base64Decode(small);
        } on FormatException {
          // Shown without a preview.
        }
      }
    }
    final task = j['task'];
    if (task is Map) {
      taskDone = task['done'] == true;
      taskDoneBy = task['by_name'] as String? ?? '';
    }
    if (type == 'm.poll') {
      body = content['question'] as String? ?? body;
      pollOptions = ((content['options'] as List?) ?? const []).map((o) => '$o').toList();
      pollMultiple = content['multiple'] == true;
    }
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
        _ => isFile && body.isEmpty ? (isImage ? 'Photo' : 'File: $fileName') : body,
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
  String description = ''; // a short line about the community
  String icon = ''; // a small picture as base64, or empty
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
    description = j['description'] as String? ?? description;
    icon = j['icon'] as String? ?? icon;
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

/// What a person says about themselves. Every field is optional. It reaches
/// the people they share a chat with, encrypted; no server sees it.
class Profile {
  Profile.fromJson(Map<String, dynamic> j)
      : displayName = j['display_name'] as String? ?? '',
        fullName = j['full_name'] as String? ?? '',
        birthday = j['birthday'] as String? ?? '',
        bio = j['bio'] as String? ?? '',
        picture = j['picture'] as String? ?? '',
        bot = j['bot'] == true,
        links = ((j['links'] as List?) ?? const []).map((e) => '$e').toList();
  final String displayName, fullName, birthday, bio;

  /// Whether this is a program, not a person. Self-declared.
  final bool bot;

  /// A small picture as base64 (JPEG or PNG), or empty.
  final String picture;
  final List<String> links;

  bool get isEmpty =>
      displayName.isEmpty &&
      fullName.isEmpty &&
      birthday.isEmpty &&
      bio.isEmpty &&
      links.isEmpty &&
      picture.isEmpty;
}
