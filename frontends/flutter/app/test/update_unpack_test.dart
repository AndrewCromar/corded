import 'dart:io';
import 'dart:isolate';

import 'package:archive/archive_io.dart';
import 'package:flutter_test/flutter_test.dart';

// The Windows update is a zip holding one folder with the app in it; the app
// unpacks it itself, away from the screen's thread.
void main() {
  test('an update zip unpacks into its one folder, files in folders included', () async {
    final work = Directory.systemTemp.createTempSync('corded-unpack');
    addTearDown(() => work.deleteSync(recursive: true));
    final made = Directory('${work.path}/make/corded-app-v9-windows-x64')..createSync(recursive: true);
    File('${made.path}/corded_app.exe').writeAsBytesSync(List.generate(70000, (i) => i % 251));
    File('${made.path}/data/flutter_assets/note.txt')
      ..createSync(recursive: true)
      ..writeAsStringSync('new');
    final zip = '${work.path}/update.zip';
    final encoder = ZipFileEncoder()..create(zip);
    await encoder.addDirectory(made);
    await encoder.close();

    final out = '${work.path}/unpacked';
    await Isolate.run(() => extractFileToDisk(zip, out));
    final fresh = Directory(out).listSync().whereType<Directory>().single;
    expect(File('${fresh.path}/corded_app.exe').lengthSync(), 70000);
    expect(File('${fresh.path}/data/flutter_assets/note.txt').readAsStringSync(), 'new');
  });
}
