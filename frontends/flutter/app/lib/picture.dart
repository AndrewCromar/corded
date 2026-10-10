// Shrinking a chosen photo to a profile picture small enough to travel inside
// the encrypted profile.
import 'dart:convert';
import 'dart:typed_data';

import 'package:image/image.dart' as img;

/// A square JPEG, 128 pixels a side, as base64. Null if the bytes are not a
/// picture this app can read.
String? shrinkToProfilePicture(Uint8List bytes) {
  final decoded = img.decodeImage(bytes);
  if (decoded == null) return null;
  final upright = img.bakeOrientation(decoded);
  final square = img.copyResizeCropSquare(upright, size: 128);
  // Lower the quality until it is comfortably small.
  for (final quality in [85, 70, 55, 40]) {
    final jpeg = img.encodeJpg(square, quality: quality);
    if (jpeg.length <= 20 * 1024) return base64Encode(jpeg);
  }
  return base64Encode(img.encodeJpg(img.copyResizeCropSquare(upright, size: 96), quality: 40));
}
