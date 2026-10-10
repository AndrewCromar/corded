// Starting the script that puts a Windows update in place. Kept apart from
// the rest of the updater, with nothing but dart:io, so that the build
// machines can run exactly this on a real Windows and see the update happen.
import 'dart:io';

/// Starts `apply-update.ps1` so that it outlives this program.
///
/// It goes through `start`, which gives the script a window of its own
/// (minimised; it closes when the script ends). Starting PowerShell detached
/// with no window at all looks neater and does not work: PowerShell never
/// runs the script, and the app closes with nothing replaced (v0.6.4 did).
Future<void> launchWindowsUpdate({
  required String script,
  required int appPid,
  required String source,
  required String target,
  required String start,
  required String log,
}) =>
    Process.start(
        'cmd',
        [
          '/c',
          'start',
          '', // the window's title, which "start" takes first
          '/min',
          'powershell',
          '-NoProfile',
          '-NonInteractive',
          '-ExecutionPolicy',
          'Bypass',
          '-File',
          script,
          '-ProcessId',
          '$appPid',
          '-Source',
          source,
          '-Target',
          target,
          '-Start',
          start,
          '-Log',
          log,
        ],
        mode: ProcessStartMode.detached);
