import 'dart:convert';
import 'dart:typed_data';

import 'package:corded_app/picture.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:image/image.dart' as img;

void main() {
  test('a large photo becomes a small square picture', () {
    final photo = img.Image(width: 1600, height: 900);
    for (var y = 0; y < photo.height; y++) {
      for (var x = 0; x < photo.width; x++) {
        photo.setPixelRgb(x, y, (x * 7) % 256, (y * 5) % 256, (x + y) % 256);
      }
    }
    final small = shrinkToProfilePicture(Uint8List.fromList(img.encodePng(photo)))!;
    final bytes = base64Decode(small);
    expect(bytes.length, lessThanOrEqualTo(20 * 1024));
    final decoded = img.decodeJpg(bytes)!;
    expect(decoded.width, decoded.height);
    expect(decoded.width, lessThanOrEqualTo(128));
  });

  test('something that is not a picture is refused', () {
    expect(shrinkToProfilePicture(Uint8List.fromList(utf8.encode('not a picture'))), isNull);
  });
}
