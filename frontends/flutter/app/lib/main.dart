import 'dart:async';

import 'package:flutter/material.dart';

import 'app_state.dart';
import 'screens/add_server.dart';
import 'screens/chat.dart';
import 'screens/home.dart';
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

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    // A message somewhere else shows as a banner at the top for a few seconds.
    state.onBanner = _showBanner;
    // A tapped notification leads to its chat, and into its thread if it has one.
    state.onOpenChat = (roomId, threadRoot) {
      final navigator = _navigator.currentState;
      if (navigator == null) return;
      navigator.popUntil((route) => route.isFirst);
      navigator.push(MaterialPageRoute(builder: (_) => ChatScreen(state: state, roomId: roomId)));
      if (threadRoot != null) {
        navigator.push(MaterialPageRoute(
            builder: (_) => ChatScreen(state: state, roomId: roomId, threadRoot: threadRoot)));
      }
    };
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
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
                            maxLines: 2,
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
    _bannerTimer = Timer(const Duration(seconds: 5), _dismissBanner);
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
        return state.store.servers.isEmpty
            ? AddServerScreen(state: state, first: true)
            : HomeScreen(state: state);
      default:
        return UnlockScreen(state: state);
    }
  }
}
