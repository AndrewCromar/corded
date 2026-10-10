// Shrinking a chosen photo to a profile picture small enough to travel inside
// the encrypted profile.
import 'dart:convert';
import 'dart:typed_data';

import 'package:image/image.dart' as img;

/// A square JPEG, 128 pixels a side, as base64. Null if the bytes are not a
/// picture this app can read.
String? shrinkToProfilePicture(Uint8List bytes) =>
    cropToProfilePicture((bytes: bytes, left: 0, top: 0, side: 0));

/// The same, from the square the person chose: [left] and [top] are where it
/// starts and [side] how wide it is, each as a share of the picture's width
/// (top as a share of its height). A side of 0 means the middle of the picture.
String? cropToProfilePicture(({Uint8List bytes, double left, double top, double side}) job) {
  final decoded = img.decodeImage(job.bytes);
  if (decoded == null) return null;
  var upright = img.bakeOrientation(decoded);
  if (job.side > 0) {
    final side = (job.side * upright.width)
        .round()
        .clamp(1, upright.width < upright.height ? upright.width : upright.height);
    final x = (job.left * upright.width).round().clamp(0, upright.width - side);
    final y = (job.top * upright.height).round().clamp(0, upright.height - side);
    upright = img.copyCrop(upright, x: x, y: y, width: side, height: side);
  }
  final square = img.copyResizeCropSquare(upright, size: 128);
  // Lower the quality until it is comfortably small.
  for (final quality in [85, 70, 55, 40]) {
    final jpeg = img.encodeJpg(square, quality: quality);
    if (jpeg.length <= 20 * 1024) return base64Encode(jpeg);
  }
  return base64Encode(img.encodeJpg(img.copyResizeCropSquare(upright, size: 96), quality: 40));
}

/// What a photo's message carries besides the photo: a small preview, as
/// base64, and the photo's shape. Null if the bytes are not a picture this
/// app can read. Heavy enough to run off the main thread.
({String thumbnail, int width, int height})? previewOf(Uint8List bytes) {
  final decoded = img.decodeImage(bytes);
  if (decoded == null) return null;
  final upright = img.bakeOrientation(decoded);
  final wide = upright.width >= upright.height;
  final small = img.copyResize(upright, width: wide ? 240 : null, height: wide ? null : 240);
  return (
    thumbnail: base64Encode(img.encodeJpg(small, quality: 55)),
    width: upright.width,
    height: upright.height
  );
}
