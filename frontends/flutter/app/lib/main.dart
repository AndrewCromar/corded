import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';

import 'platform.dart';
import 'app_state.dart';
import 'screens/add_server.dart';
import 'screens/chat.dart';
import 'screens/home.dart';
import 'screens/settings.dart';
import 'screens/unlock.dart';
import 'screens/welcome.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  final state = AppState()..start();
  runApp(CordedApp(state: state));
}

class CordedApp extends StatefulWidget {
  const CordedApp({super.key, required this.state});
  final AppState state;

  @override
  State<CordedApp> createState() => _CordedAppState();
}

class _CordedAppState extends State<CordedApp> with WidgetsBindingObserver {
  AppState get state => widget.state;

  final _navigator = GlobalKey<NavigatorState>();

  // On a desktop window wide enough, the chats stay listed on the left and
  // the open one fills the rest, instead of one screen covering the other.
  bool _wide = false;
  String? _paneRoom;

  // For checking the look of the app where no one can see its window: with
  // CORDED_SCREENSHOT=/some/file.png set, the app saves a picture of itself
  // there every few seconds.
  final _whole = GlobalKey();
  Timer? _camera;

  Future<void> _photograph(String path) async {
    try {
      final boundary = _whole.currentContext?.findRenderObject() as RenderRepaintBoundary?;
      if (boundary == null) return;
      final image = await boundary.toImage();
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      if (bytes != null) await File(path).writeAsBytes(bytes.buffer.asUint8List());
    } catch (_) {
      // Only a development aid.
    }
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    // A message somewhere else shows as a banner at the top for a few seconds.
    state.onBanner = _showBanner;
    // A tapped notification leads to its chat, and into its thread if it has one.
    state.onDevScreen = (screen) {
      if (screen == 'settings') {
        _navigator.currentState?.push(MaterialPageRoute(builder: (_) => SettingsScreen(state: state)));
      }
    };
    final shot = Platform.environment['CORDED_SCREENSHOT'];
    if (shot != null && shot.isNotEmpty) {
      _camera = Timer.periodic(const Duration(seconds: 3), (_) => _photograph(shot));
    }
    state.onOpenChat = (roomId, threadRoot) {
      final navigator = _navigator.currentState;
      if (navigator == null) return;
      navigator.popUntil((route) => route.isFirst);
      if (_wide) {
        setState(() => _paneRoom = roomId);
      } else {
        navigator.push(MaterialPageRoute(builder: (_) => ChatScreen(state: state, roomId: roomId)));
      }
      if (threadRoot != null) {
        navigator.push(MaterialPageRoute(
            builder: (_) => ChatScreen(state: state, roomId: roomId, threadRoot: threadRoot)));
      }
    };
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _camera?.cancel();
    super.dispose();
  }

  OverlayEntry? _banner;
  Timer? _bannerTimer;

  void _dismissBanner() {
    _bannerTimer?.cancel();
    _banner?.remove();
    _banner = null;
  }

  void _showBanner(String title, String body, String roomId, String? threadRoot) {
    final overlay = _navigator.currentState?.overlay;
    if (overlay == null) return;
    _dismissBanner();
    final entry = OverlayEntry(
      builder: (context) {
        final scheme = Theme.of(context).colorScheme;
        return Positioned(
          top: MediaQuery.paddingOf(context).top + 8,
          left: 12,
          right: 12,
          child: Dismissible(
            key: UniqueKey(),
            direction: DismissDirection.up,
            onDismissed: (_) => _dismissBanner(),
            child: Material(
              elevation: 6,
              color: scheme.surfaceContainerHighest,
              shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(14),
                  side: BorderSide(color: scheme.primary, width: 1.5)),
              child: InkWell(
                borderRadius: BorderRadius.circular(14),
                onTap: () {
                  _dismissBanner();
                  // A notice from the server leads nowhere; a message leads to its chat.
                  if (roomId.isEmpty) return;
                  state.openFromNotification(
                      {'room_id': roomId, if (threadRoot != null) 'thread': threadRoot});
                },
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
                  child: Row(children: [
                    Icon(Icons.chat_bubble_outline, color: scheme.onSurface, size: 20),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                        Text(title,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: TextStyle(color: scheme.onSurface, fontWeight: FontWeight.bold)),
                        Text(body,
                            maxLines: roomId.isEmpty ? 5 : 2,
                            overflow: TextOverflow.ellipsis,
                            style: TextStyle(color: scheme.onSurface)),
                      ]),
                    ),
                  ]),
                ),
              ),
            ),
          ),
        );
      },
    );
    _banner = entry;
    overlay.insert(entry);
    _bannerTimer = Timer(Duration(seconds: roomId.isEmpty ? 9 : 5), _dismissBanner);
  }

  // Notifications are for when nobody is looking at the app.
  @override
  void didChangeAppLifecycleState(AppLifecycleState lifecycle) =>
      state.setOnScreen(lifecycle == AppLifecycleState.resumed);

  @override
  Widget build(BuildContext context) {
    ThemeData theme(Brightness b) => ThemeData(
          colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFF4F6BED), brightness: b),
          useMaterial3: true,
        );
    return MaterialApp(
      title: 'Corded',
      navigatorKey: _navigator,
      theme: theme(Brightness.light),
      darkTheme: theme(Brightness.dark),
      builder: (context, child) => RepaintBoundary(key: _whole, child: child),
      home: ListenableBuilder(listenable: state, builder: (context, _) => _root()),
    );
  }

  // Which screen the app rests on follows from the state of the vault.
  Widget _root() {
    if (state.startError != null) {
      return Scaffold(
          body: Center(
              child: Padding(
                  padding: const EdgeInsets.all(24),
                  child:
                      Text('Corded could not start.\n\n${state.startError}', textAlign: TextAlign.center))));
    }
    if (!state.ready) return const Scaffold(body: Center(child: CircularProgressIndicator()));
    switch (state.store.vaultState) {
      case 'missing':
        return WelcomeScreen(state: state);
      case 'unlocked':
        if (state.store.servers.isEmpty) return AddServerScreen(state: state, first: true);
        return LayoutBuilder(builder: (context, box) {
          _wide = isDesktop && box.maxWidth >= 840;
          if (!_wide) return HomeScreen(state: state);
          final open = _paneRoom != null && state.store.rooms.containsKey(_paneRoom) ? _paneRoom : null;
          return Row(children: [
            SizedBox(width: 340, child: HomeScreen(state: state)),
            const VerticalDivider(width: 1),
            Expanded(
              child: open == null
                  ? const Scaffold(body: Center(child: Text('Choose a chat on the left.')))
                  : ChatScreen(key: ValueKey(open), state: state, roomId: open),
            ),
          ]);
        });
      default:
        return UnlockScreen(state: state);
    }
  }
}
