// For the build machines: starts the update script the way the app does and
// then ends at once, as the app does.
//   dart tool/launch_update.dart <script> <pid to wait for> <source> <target> <start> <log>
import 'dart:io';

// Run with plain `dart`, before any packages are fetched, so by its path.
// ignore: avoid_relative_lib_imports
import '../lib/update_launch.dart';

Future<void> main(List<String> a) async {
  await launchWindowsUpdate(
      script: a[0], appPid: int.parse(a[1]), source: a[2], target: a[3], start: a[4], log: a[5]);
  Process.killPid(pid, ProcessSignal.sigkill);
  exit(0);
}
