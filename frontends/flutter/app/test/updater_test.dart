import 'package:corded_app/updater.dart';
import 'package:flutter_test/flutter_test.dart';

Map<String, dynamic> release(String tag, List<String> files, {bool draft = false}) => {
      'tag_name': tag,
      'draft': draft,
      'assets': [
        for (final f in files) {'name': f, 'browser_download_url': 'https://example.com/$tag/$f'}
      ],
    };

void main() {
  test('versions compare by their numbers, and what git adds after the tag is ignored', () {
    expect(isNewer('v0.6.2', 'v0.6.1'), isTrue);
    expect(isNewer('v0.10.0', 'v0.9.9'), isTrue);
    expect(isNewer('v0.6.1', 'v0.6.1-3-gabc1234'), isFalse);
    expect(isNewer('v0.6.1', 'v0.6.2'), isFalse);
    expect(isNewer('v0.6.1', 'dev'), isTrue); // a build made by hand is older than any release
    expect(isNewer('android-test-42', 'v0.1.0'), isFalse);
  });

  test('the newest release with a file for this device is offered, with its signature', () {
    final releases = [
      release('android-test-50', ['corded-android-arm64.apk']),
      release('v0.7.0', ['corded-v0.7.0-linux-x86_64.tar.gz']), // server only: nothing for the app
      release('v0.6.2', [
        'corded-app-v0.6.2-linux-x86_64.tar.gz',
        'corded-app-v0.6.2-linux-x86_64.tar.gz.sig',
        'corded-app-v0.6.2-windows-x64.zip',
        'corded-android-arm64.apk',
        'corded-android-arm64.apk.sig',
      ]),
      release('v0.6.1', ['corded-app-v0.6.1-linux-x86_64.tar.gz']),
    ];
    final linux = pickUpdate(releases, 'v0.6.1', platform: 'linux')!;
    expect(linux.tag, 'v0.6.2');
    expect(linux.url, endsWith('/corded-app-v0.6.2-linux-x86_64.tar.gz'));
    expect(linux.signatureUrl, endsWith('.tar.gz.sig'));
    // Offered, but it has no signature, so installing it will be refused.
    expect(pickUpdate(releases, 'v0.6.1', platform: 'windows')!.signatureUrl, isNull);
    expect(pickUpdate(releases, 'v0.6.1-9-gabc', platform: 'android')!.name, 'corded-android-arm64.apk');
    // Already on it, or ahead of it: nothing to do.
    expect(pickUpdate(releases, 'v0.6.2', platform: 'linux'), isNull);
    expect(pickUpdate(releases, 'v0.9.0', platform: 'linux'), isNull);
    // A kind of device the app cannot update on.
    expect(pickUpdate(releases, 'v0.6.1', platform: 'macos'), isNull);
    expect(
        pickUpdate([
          release('v0.8.0', ['corded-android-arm64.apk'], draft: true)
        ], 'v0.1.0', platform: 'android'),
        isNull);
  });
}
