// Notifications on a desktop computer. There the app keeps its connection for
// as long as its window is open, so it announces messages itself whenever the
// window is not the one being looked at; no background service is involved.
import 'dart:convert';

import 'package:flutter_local_notifications/flutter_local_notifications.dart';

import 'platform.dart';

class DesktopNotifier {
  static final _plugin = FlutterLocalNotificationsPlugin();
  static bool _ready = false;

  /// [onTap] is given where a clicked notification leads ({room_id, thread}).
  static Future<void> init(void Function(Map<String, dynamic>) onTap) async {
    if (!isDesktop || _ready) return;
    try {
      await _plugin.initialize(
        const InitializationSettings(
          linux: LinuxInitializationSettings(defaultActionName: 'Open'),
          windows: WindowsInitializationSettings(
              appName: 'Corded',
              appUserModelId: 'org.corded.app',
              guid: '6f0b1c8e-52a4-4d0e-9c2b-7a3f6d1e4b90'),
        ),
        onDidReceiveNotificationResponse: (response) {
          try {
            onTap((jsonDecode(response.payload ?? '{}') as Map).cast<String, dynamic>());
          } catch (_) {
            // A notification without a place to go.
          }
        },
      );
      _ready = true;
    } catch (_) {
      // No notification service on this desktop; the app works without.
    }
  }

  static void show(({String roomId, String title, String body, String payload}) n) {
    if (!_ready) return;
    _plugin
        .show(
          n.roomId.hashCode & 0x7fffffff,
          n.title,
          n.body,
          const NotificationDetails(linux: LinuxNotificationDetails(), windows: WindowsNotificationDetails()),
          payload: n.payload,
        )
        .catchError((_) {});
  }
}
