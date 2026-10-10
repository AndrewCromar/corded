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

  test('a photo to send gets a small preview and keeps its shape', () {
    final photo = img.Image(width: 1200, height: 800);
    final p = previewOf(Uint8List.fromList(img.encodeJpg(photo)))!;
    expect((p.width, p.height), (1200, 800));
    final small = img.decodeJpg(base64Decode(p.thumbnail))!;
    expect((small.width, small.height), (240, 160));
    expect(previewOf(Uint8List.fromList(utf8.encode('not a picture'))), isNull);
  });

  test('the chosen square of a picture is what gets kept', () {
    // Left half red, right half blue, twice as wide as tall.
    final photo = img.Image(width: 400, height: 200);
    for (var y = 0; y < 200; y++) {
      for (var x = 0; x < 400; x++) {
        photo.setPixelRgb(x, y, x < 200 ? 255 : 0, 0, x < 200 ? 0 : 255);
      }
    }
    final bytes = Uint8List.fromList(img.encodePng(photo));
    img.Color middle(String? small) {
      final picture = img.decodeJpg(base64Decode(small!))!;
      return picture.getPixel(picture.width ~/ 2, picture.height ~/ 2);
    }

    // The right-hand square is blue; the left-hand one is red.
    final right = middle(cropToProfilePicture((bytes: bytes, left: 0.5, top: 0, side: 0.5)));
    expect(right.b, greaterThan(200));
    expect(right.r, lessThan(60));
    final left = middle(cropToProfilePicture((bytes: bytes, left: 0, top: 0, side: 0.5)));
    expect(left.r, greaterThan(200));
    // A square that hangs over the edge is pulled back inside.
    expect(cropToProfilePicture((bytes: bytes, left: 0.9, top: 0.9, side: 0.5)), isNotNull);
  });

  test('something that is not a picture is refused', () {
    expect(shrinkToProfilePicture(Uint8List.fromList(utf8.encode('not a picture'))), isNull);
  });
}
