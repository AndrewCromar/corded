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
