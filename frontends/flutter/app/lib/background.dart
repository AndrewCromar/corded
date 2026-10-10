// Staying connected while the app is not on screen.
//
// Android stops an app's network use soon after it leaves the screen unless
// it runs a foreground service, which shows a permanent notification. With
// the service on, the core keeps its connections, and a small task that lives
// with the service reads the core's events: it shows a notification for each
// new message while nobody is looking, and passes every event on to the
// screen whenever there is one.
import 'dart:convert';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

const _engineKey = 'engine_address';

@pragma('vm:entry-point')
void backgroundEntry() {
  FlutterForegroundTask.setTaskHandler(_EventReader());
}

/// What a notification says for one event, or null if it deserves none.
/// Only messages written by someone else are announced.
({String roomId, String title, String body, String payload})? notificationFor(
    Map<String, dynamic> event, Map<String, String> roomTitles,
    {Set<String> muted = const {}, bool showText = true, String me = '', Set<String> nsfw = const {}}) {
  if (event['event'] != 'event_received') return null;
  final data = (event['data'] as Map?)?.cast<String, dynamic>() ?? const {};
  final isPoll = data['type'] == 'm.poll',
      isFile = data['type'] == 'm.file',
      isTask = data['type'] == 'm.task';
  if ((data['type'] != 'm.text' && !isPoll && !isFile && !isTask) ||
      data['mine'] == true ||
      data['shared_history'] == true) {
    return null;
  }
  final content = (data['content'] as Map?) ?? const {};
  // A file is announced by its caption, or by what it is.
  final body = isPoll
      ? 'Poll: ${content['question'] ?? ''}'
      : isTask
          ? 'New task: ${content['body'] ?? ''}'
          : isFile && (content['body'] is! String || (content['body'] as String).isEmpty)
              ? ('${content['mime'] ?? ''}'.startsWith('image/')
                  ? 'Sent a photo'
                  : 'Sent a file: ${content['name'] ?? ''}')
              : content['body'];
  if (body is! String || body.isEmpty) return null;
  final roomId = '${data['room_id']}';
  // A mention gets through even from a muted chat.
  final mentioned = mentionsUser(body, me);
  if (muted.contains(roomId) && !mentioned) return null;
  final sender = '${data['sender_name'] ?? 'Someone'}';
  final title = roomTitles[roomId] ?? sender;
  // In a direct chat the title is already the sender's name.
  // Where a tap should lead: the chat, and the thread if it was said in one.
  final relation = (data['relation'] as Map?) ?? const {};
  final payload = jsonEncode({
    'room_id': roomId,
    if (relation['kind'] == 'thread') 'thread': relation['target'],
  });
  // Hidden text still says who wrote and where, never what.
  // An NSFW channel's words never appear in a notification.
  if (nsfw.contains(roomId)) showText = false;
  final shown = !showText
      ? (mentioned ? '$sender mentioned you' : (title == sender ? 'New message' : 'New message from $sender'))
      : (title == sender ? body : '$sender: $body');
  if (mentioned && title != sender) {
    return (roomId: roomId, title: '$title · mentioned you', body: shown, payload: payload);
  }
  return (roomId: roomId, title: title, body: shown, payload: payload);
}

class _EventReader extends TaskHandler {
  int _engine = 0;
  bool _onScreen = true;
  bool _doNotDisturb = false;
  Set<String> _muted = {};
  String _me = '';
  Set<String> _nsfw = {};
  bool _showText = true;
  final _roomTitles = <String, String>{};
  final _notifications = FlutterLocalNotificationsPlugin();
  bool _ready = false;

  @override
  Future<void> onStart(DateTime timestamp, TaskStarter starter) async {
    _engine = await FlutterForegroundTask.getData<int>(key: _engineKey) ?? 0;
    try {
      await _notifications.initialize(
          const InitializationSettings(android: AndroidInitializationSettings('@mipmap/ic_launcher')));
      _ready = true;
    } catch (_) {
      // Without it the connection is still kept; only the notices are missing.
    }
  }

  @override
  void onRepeatEvent(DateTime timestamp) {
    if (_engine == 0) return;
    for (final json in CordedEngine.drain(_engine)) {
      FlutterForegroundTask.sendDataToMain(json);
      final Map<String, dynamic> event;
      try {
        event = jsonDecode(json) as Map<String, dynamic>;
      } catch (_) {
        continue;
      }
      if (event['event'] == 'room_updated') {
        final room = (event['room'] as Map?) ?? const {};
        _roomTitles['${room['room_id']}'] = '${room['title']}';
      }
      if (_onScreen || _doNotDisturb || !_ready) continue;
      final n = notificationFor(event, _roomTitles, muted: _muted, showText: _showText, me: _me, nsfw: _nsfw);
      if (n == null) continue;
      _notifications.show(
        n.roomId.hashCode & 0x7fffffff,
        n.title,
        n.body,
        const NotificationDetails(
          android: AndroidNotificationDetails('messages', 'Messages',
              channelDescription: 'New messages while Corded is not on screen',
              importance: Importance.high,
              priority: Priority.high),
        ),
        payload: n.payload,
      );
    }
  }

  @override
  void onReceiveData(Object data) {
    // The screen says whether someone is looking at it.
    if (data == 'on_screen') {
      _onScreen = true;
      if (_ready) _notifications.cancelAll();
    }
    if (data == 'off_screen') _onScreen = false;
    if (data == 'dnd_on') _doNotDisturb = true;
    if (data == 'dnd_off') _doNotDisturb = false;
    if (data is Map && data['muted'] is List) {
      _muted = (data['muted'] as List).map((e) => '$e').toSet();
      if (data['show_text'] is bool) _showText = data['show_text'] as bool;
      if (data['me'] is String) _me = data['me'] as String;
      if (data['nsfw'] is List) _nsfw = (data['nsfw'] as List).map((e) => '$e').toSet();
    }
    if (data is Map && data['engine'] is int) _engine = data['engine'] as int;
    // Names of the chats, for the titles of notifications.
    if (data is Map && data['rooms'] is Map) {
      (data['rooms'] as Map).forEach((id, title) => _roomTitles['$id'] = '$title');
    }
  }

  @override
  Future<void> onDestroy(DateTime timestamp, bool isTimeout) async {}
}

/// Starting and stopping the service, from the screen's side.
class Background {
  static bool get supported => _supported;
  static bool _supported = false;

  static void init() {
    try {
      FlutterForegroundTask.initCommunicationPort();
      FlutterForegroundTask.init(
        androidNotificationOptions: AndroidNotificationOptions(
          // As quiet as Android allows: no status-bar icon, bottom of the
          // shade. Android fixes a category's loudness when it is first
          // made, hence the new name.
          channelId: 'connection_quiet',
          channelName: 'Connection',
          channelDescription: 'Shown while Corded stays connected in the background. '
              'You can turn this category off; message notifications are separate.',
          channelImportance: NotificationChannelImportance.MIN,
          priority: NotificationPriority.MIN,
          showBadge: false,
          onlyAlertOnce: true,
        ),
        iosNotificationOptions: const IOSNotificationOptions(showNotification: false, playSound: false),
        foregroundTaskOptions: ForegroundTaskOptions(
          eventAction: ForegroundTaskEventAction.repeat(400),
          autoRunOnBoot: false,
          autoRunOnMyPackageReplaced: false,
          allowWakeLock: true,
          allowWifiLock: true,
        ),
      );
      _supported = true;
    } catch (_) {
      _supported = false; // not a phone
    }
  }

  static Future<bool> get running async {
    if (!_supported) return false;
    try {
      return await FlutterForegroundTask.isRunningService;
    } catch (_) {
      return false;
    }
  }

  /// Asks for what Android requires, then starts the service reading events
  /// from the core at [engineAddress]. Returns a sentence saying why not, or
  /// null when it is running.
  static Future<String?> start(int engineAddress) async {
    if (!_supported) return 'This device cannot keep the app connected in the background.';
    if (await FlutterForegroundTask.checkNotificationPermission() != NotificationPermission.granted) {
      await FlutterForegroundTask.requestNotificationPermission();
    }
    if (await FlutterForegroundTask.checkNotificationPermission() != NotificationPermission.granted) {
      return 'Corded needs permission to show notifications to stay connected in the background.';
    }
    await FlutterForegroundTask.saveData(key: _engineKey, value: engineAddress);
    final ServiceRequestResult result;
    if (await FlutterForegroundTask.isRunningService) {
      result = await FlutterForegroundTask.restartService();
    } else {
      result = await FlutterForegroundTask.startService(
        serviceId: 7443,
        serviceTypes: [ForegroundServiceTypes.remoteMessaging],
        notificationTitle: 'Corded',
        notificationText: 'Connected',
        callback: backgroundEntry,
      );
    }
    if (result is ServiceRequestFailure) {
      return 'Android would not start the background connection: ${result.error}';
    }
    return null;
  }

  static Future<void> stop() async {
    if (!_supported) return;
    try {
      await FlutterForegroundTask.stopService();
    } catch (_) {
      // Already gone.
    }
  }

  /// Without this, Android may still pause the connection when the phone has
  /// been still for a long time.
  static Future<void> askToRunUnrestricted() async {
    if (!_supported) return;
    try {
      if (!await FlutterForegroundTask.isIgnoringBatteryOptimizations) {
        await FlutterForegroundTask.requestIgnoreBatteryOptimization();
      }
    } catch (_) {
      // Optional.
    }
  }

  static void tell(Object message) {
    if (!_supported) return;
    try {
      FlutterForegroundTask.sendDataToTask(message);
    } catch (_) {
      // No task to tell.
    }
  }

  static void onEvent(void Function(Object) callback) => FlutterForegroundTask.addTaskDataCallback(callback);

  /// Calls [onTap] with where a tapped notification leads ({room_id, thread}),
  /// including the one that started the app, if a notification did.
  static Future<void> listenForTaps(void Function(Map<String, dynamic>) onTap) async {
    if (!_supported) return;
    void handle(String? payload) {
      if (payload == null || payload.isEmpty) return;
      try {
        onTap((jsonDecode(payload) as Map).cast<String, dynamic>());
      } catch (_) {
        // Not one of ours.
      }
    }

    try {
      final plugin = FlutterLocalNotificationsPlugin();
      await plugin.initialize(
        const InitializationSettings(android: AndroidInitializationSettings('@mipmap/ic_launcher')),
        onDidReceiveNotificationResponse: (response) => handle(response.payload),
      );
      final launch = await plugin.getNotificationAppLaunchDetails();
      if (launch?.didNotificationLaunchApp ?? false) handle(launch!.notificationResponse?.payload);
    } catch (_) {
      // Taps then only bring the app forward.
    }
  }
}
