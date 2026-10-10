// Updating the app from inside the app.
//
// Releases are published on GitHub. The app asks which is newest, downloads
// the file for this kind of device, and installs it only if it carries the
// release key's signature (checked by the core, which holds the public key).
// A phone hands the new APK to Android, which asks the person to confirm; a
// desktop app replaces its own files and starts itself again.
import 'dart:convert';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:archive/archive_io.dart';
import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/services.dart' show rootBundle;
import 'package:open_filex/open_filex.dart';
import 'package:path_provider/path_provider.dart';

import 'update_launch.dart';

/// Which release this build is, stamped in when it was built ("v0.6.1", or
/// "v0.6.1-3-gabc1234" for a build made after it). "dev" for a build made by hand.
const appVersion = String.fromEnvironment('CORDED_VERSION', defaultValue: 'dev');

const _repository = 'AndrewCromar/corded';

class Update {
  const Update({required this.tag, required this.name, required this.url, required this.signatureUrl});
  final String tag, name, url;
  final String? signatureUrl;
}

/// "v0.6.1-3-gabc1234" and "v0.6.1" are the same release.
String releaseOf(String version) => version.split('-').first;

List<int>? _numbers(String tag) {
  final m = RegExp(r'^v(\d+)\.(\d+)\.(\d+)$').firstMatch(tag);
  return m == null ? null : [for (var i = 1; i <= 3; i++) int.parse(m.group(i)!)];
}

/// Whether release [a] is newer than release [b]. A version that cannot be
/// read (a build made by hand) counts as older than any release.
bool isNewer(String a, String b) {
  final x = _numbers(releaseOf(a)), y = _numbers(releaseOf(b));
  if (x == null) return false;
  if (y == null) return true;
  for (var i = 0; i < 3; i++) {
    if (x[i] != y[i]) return x[i] > y[i];
  }
  return false;
}

/// The name of this platform's file in release [tag], or null where the app
/// cannot update itself.
String? assetName(String tag, {String? platform}) => switch (platform ?? Platform.operatingSystem) {
      'android' => 'corded-android-arm64.apk',
      'linux' => 'corded-app-$tag-linux-x86_64.tar.gz',
      'windows' => 'corded-app-$tag-windows-x64.zip',
      _ => null,
    };

/// Out of GitHub's list of releases (newest first), the newest one that is
/// newer than [current] and has a file for [platform]. Null if there is none.
Update? pickUpdate(List<dynamic> releases, String current, {String? platform}) {
  for (final r in releases) {
    if (r is! Map || r['draft'] == true) continue;
    final tag = '${r['tag_name'] ?? ''}';
    if (_numbers(tag) == null) continue; // test builds and the like
    final wanted = assetName(tag, platform: platform);
    if (wanted == null) return null;
    String? url, signature;
    for (final a in (r['assets'] as List? ?? const [])) {
      if (a is! Map) continue;
      if (a['name'] == wanted) url = a['browser_download_url'] as String?;
      if (a['name'] == '$wanted.sig') signature = a['browser_download_url'] as String?;
    }
    if (url == null) continue; // that release has nothing for this device
    return isNewer(tag, current) ? Update(tag: tag, name: wanted, url: url, signatureUrl: signature) : null;
  }
  return null;
}

Future<List<int>> _get(HttpClient client, Uri uri, {void Function(int got, int total)? progress}) async {
  final request = await client.getUrl(uri);
  request.headers.set(HttpHeaders.userAgentHeader, 'corded-app');
  request.headers.set(HttpHeaders.acceptHeader, 'application/vnd.github+json, application/octet-stream');
  final response = await request.close();
  if (response.statusCode != 200) {
    await response.drain<void>();
    throw CordedError('update', 'GitHub answered ${response.statusCode}.');
  }
  final out = BytesBuilder(copy: false);
  await for (final chunk in response) {
    out.add(chunk);
    progress?.call(out.length, response.contentLength);
  }
  return out.takeBytes();
}

/// Asks GitHub whether there is a newer release for this device.
Future<Update?> checkForUpdate() async {
  if (assetName('v0.0.0') == null) return null;
  final client = HttpClient()..connectionTimeout = const Duration(seconds: 10);
  try {
    final body =
        await _get(client, Uri.parse('https://api.github.com/repos/$_repository/releases?per_page=20'))
            .timeout(const Duration(seconds: 20));
    final releases = jsonDecode(utf8.decode(body));
    return releases is List ? pickUpdate(releases, appVersion) : null;
  } finally {
    client.close(force: true);
  }
}

/// Downloads [update], checks its signature, and returns where it was saved.
/// Throws a [CordedError] with a sentence for the person if anything is off.
Future<String> downloadUpdate(CordedEngine engine, Update update,
    {void Function(int got, int total)? progress}) async {
  final signatureUrl = update.signatureUrl;
  if (signatureUrl == null) {
    throw CordedError('update', '${update.tag} is not signed, so it was not installed.');
  }
  final folder = Directory('${(await getTemporaryDirectory()).path}/updates');
  if (folder.existsSync()) folder.deleteSync(recursive: true);
  folder.createSync(recursive: true);
  final file = File('${folder.path}/${update.name}'), signature = File('${file.path}.sig');
  final client = HttpClient()..connectionTimeout = const Duration(seconds: 15);
  try {
    signature.writeAsBytesSync(await _get(client, Uri.parse(signatureUrl)));
    file.writeAsBytesSync(await _get(client, Uri.parse(update.url), progress: progress));
  } finally {
    client.close(force: true);
  }
  final checked =
      await engine.command({'cmd': 'verify_release', 'path': file.path, 'signature_path': signature.path});
  if (checked['valid'] != true) {
    file.deleteSync();
    throw CordedError('update', 'The download did not carry the release signature. Nothing was installed.');
  }
  return file.path;
}

/// Where the Windows update script writes what it did.
String updateLogPath() => '${Directory.systemTemp.path}${Platform.pathSeparator}corded-update.log';

/// What went wrong the last time an update was put in place, or null if
/// nothing did (or nothing was tried). Read once, at start.
String? lastUpdateFailure() {
  try {
    final log = File(updateLogPath());
    if (!log.existsSync()) return null;
    final text = log.readAsStringSync();
    log.deleteSync();
    return RegExp(r'FAILED: (.*)').firstMatch(text)?.group(1)?.trim();
  } catch (_) {
    return null;
  }
}

/// Puts a checked download in place. On a phone this opens Android's own
/// installer and returns. On a desktop it replaces the app's files, starts the
/// new app and ends this one, so it does not return.
Future<void> installUpdate(String path) async {
  if (Platform.isAndroid) {
    final opened = await OpenFilex.open(path, type: 'application/vnd.android.package-archive');
    if (opened.type != ResultType.done) {
      throw CordedError('update', 'Android would not open the installer: ${opened.message}');
    }
    return;
  }
  final program = File(Platform.resolvedExecutable);
  final home = program.parent;
  final unpacked = Directory('${File(path).parent.path}/unpacked');
  if (unpacked.existsSync()) unpacked.deleteSync(recursive: true);
  unpacked.createSync(recursive: true);
  if (Platform.isLinux) {
    final tar = await Process.run('tar', ['-xzf', path, '-C', unpacked.path]);
    if (tar.exitCode != 0) throw CordedError('update', 'The update could not be unpacked.');
    final fresh = unpacked.listSync().whereType<Directory>().firstOrNull;
    if (fresh == null || !File('${fresh.path}/corded_app').existsSync()) {
      throw CordedError('update', 'The update does not hold the app.');
    }
    // Each file is written beside its place and moved over it. A program
    // that is running keeps its old file until it ends, so this is safe.
    for (final entity in fresh.listSync(recursive: true)) {
      final relative = entity.path.substring(fresh.path.length + 1);
      if (entity is Directory) {
        Directory('${home.path}/$relative').createSync(recursive: true);
      } else if (entity is File) {
        final target = File('${home.path}/$relative');
        target.parent.createSync(recursive: true);
        final staged = File('${target.path}.new');
        entity.copySync(staged.path);
        final mode = entity.statSync().mode & 0x1ff;
        await Process.run('chmod', [mode.toRadixString(8), staged.path]);
        staged.renameSync(target.path);
      }
    }
    await Process.start(program.path, const [], mode: ProcessStartMode.detached);
    exit(0);
  }
  if (Platform.isWindows) {
    // Unpacked here, in the app: starting a program for it would put a
    // terminal window on the screen for as long as the unpacking takes.
    String? problem;
    try {
      await Isolate.run(() => extractFileToDisk(path, unpacked.path));
    } catch (e) {
      problem = '$e';
    }
    final fresh = unpacked.listSync().whereType<Directory>().firstOrNull;
    if (problem != null || fresh == null || !File('${fresh.path}\\corded_app.exe').existsSync()) {
      throw CordedError('update', 'The update could not be unpacked. ${problem ?? ''}'.trim());
    }
    // Windows will not replace a program while it runs: a script waits for
    // this app to end, copies the new files over the old, and starts it again.
    final script = File('${File(path).parent.path}\\apply-update.ps1');
    script.writeAsStringSync(await rootBundle.loadString('assets/apply-update.ps1'));
    await launchWindowsUpdate(
        script: script.path,
        appPid: pid,
        source: fresh.path,
        target: home.path,
        start: program.path,
        log: updateLogPath());
    // The app has to be gone for its files to be replaced. On Windows an
    // ordinary exit can leave the window standing (the first version of this
    // did, and the update waited on an app that never left), so the process
    // is ended outright; the vault is a database that is safe to stop this way.
    Process.killPid(pid, ProcessSignal.sigkill);
    exit(0);
  }
  throw CordedError('update', 'This kind of device cannot update itself yet.');
}
