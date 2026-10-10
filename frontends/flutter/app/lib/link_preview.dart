// Link previews, made by the sender. The device of the person who writes a
// link fetches the page and attaches a small card to the message: a title, a
// line of description and a small picture. Readers are shown the card as it
// came; their devices contact nobody.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:image/image.dart' as img;

/// What travels inside a message for one link.
class LinkPreview {
  const LinkPreview({required this.url, this.title = '', this.description = '', this.image});
  final String url, title, description;

  /// A small JPEG, or null.
  final Uint8List? image;

  bool get isEmpty => title.isEmpty && description.isEmpty && image == null;

  Map<String, dynamic> toJson() => {
        'url': url,
        if (title.isNotEmpty) 'title': title,
        if (description.isNotEmpty) 'description': description,
        if (image != null) 'image': base64Encode(image!),
      };

  /// Reads a card out of a message's content; null if there is none worth showing.
  static LinkPreview? fromJson(Object? j) {
    if (j is! Map) return null;
    final url = j['url'];
    if (url is! String || !isWebAddress(url)) return null;
    Uint8List? image;
    final encoded = j['image'];
    if (encoded is String && encoded.isNotEmpty && encoded.length < 80 * 1024) {
      try {
        image = base64Decode(encoded);
      } on FormatException {
        // Shown without a picture.
      }
    }
    final preview = LinkPreview(
        url: url,
        title: _clip('${j['title'] ?? ''}', 140),
        description: _clip('${j['description'] ?? ''}', 240),
        image: image);
    return preview.isEmpty ? null : preview;
  }
}

bool isWebAddress(String text) {
  final uri = Uri.tryParse(text);
  return uri != null && (uri.scheme == 'http' || uri.scheme == 'https') && uri.host.isNotEmpty;
}

final _address = RegExp(r'https?://[^\s<>]+', caseSensitive: false);

/// The first web address in what someone typed, without punctuation that
/// only ends the sentence around it.
String? firstLink(String text) {
  final match = _address.firstMatch(text);
  if (match == null) return null;
  final link = match.group(0)!.replaceFirst(RegExp(r'[.,;:!?)\]]+$'), '');
  return isWebAddress(link) ? link : null;
}

String _clip(String text, int max) {
  final tidy = text.replaceAll(RegExp(r'\s+'), ' ').trim();
  return tidy.length <= max ? tidy : '${tidy.substring(0, max - 1)}…';
}

String _unescape(String text) => text
    .replaceAll('&amp;', '&')
    .replaceAll('&lt;', '<')
    .replaceAll('&gt;', '>')
    .replaceAll('&quot;', '"')
    .replaceAll('&#39;', "'")
    .replaceAll('&#x27;', "'")
    .replaceAll('&nbsp;', ' ');

/// What a page says about itself: its title, description and picture address.
/// Pages describe themselves in `<meta>` tags; the plain `<title>` is the fallback.
({String title, String description, String image}) readPage(String html) {
  final meta = <String, String>{};
  for (final tag in RegExp(r'<meta\b[^>]*>', caseSensitive: false).allMatches(html)) {
    final text = tag.group(0)!;
    String? attribute(String name) {
      final m = RegExp('\\b$name\\s*=\\s*(?:"([^"]*)"|\'([^\']*)\')', caseSensitive: false).firstMatch(text);
      return m == null ? null : (m.group(1) ?? m.group(2));
    }

    final key = (attribute('property') ?? attribute('name'))?.toLowerCase();
    final value = attribute('content');
    if (key != null && value != null && value.isNotEmpty) meta.putIfAbsent(key, () => _unescape(value));
  }
  final plainTitle = RegExp(r'<title[^>]*>([^<]*)</title>', caseSensitive: false).firstMatch(html)?.group(1);
  return (
    title: _clip(meta['og:title'] ?? meta['twitter:title'] ?? _unescape(plainTitle ?? ''), 140),
    description:
        _clip(meta['og:description'] ?? meta['twitter:description'] ?? meta['description'] ?? '', 240),
    image: meta['og:image'] ?? meta['twitter:image'] ?? '',
  );
}

/// A picture made small enough to ride inside a message, or null.
Uint8List? shrinkPreviewImage(Uint8List bytes) {
  final decoded = img.decodeImage(bytes);
  if (decoded == null) return null;
  final wide = decoded.width >= decoded.height;
  final small = decoded.width <= 200 && decoded.height <= 200
      ? decoded
      : img.copyResize(decoded, width: wide ? 200 : null, height: wide ? null : 200);
  return Uint8List.fromList(img.encodeJpg(small, quality: 60));
}

Future<Uint8List?> _fetch(HttpClient client, Uri uri, int limit, {required bool wantHtml}) async {
  final request = await client.getUrl(uri);
  request.followRedirects = true;
  request.maxRedirects = 5;
  request.headers.set(HttpHeaders.userAgentHeader, 'Mozilla/5.0 (compatible; CordedLinkPreview/1)');
  request.headers.set(HttpHeaders.acceptHeader, wantHtml ? 'text/html' : 'image/*');
  final response = await request.close();
  final kind = response.headers.contentType?.mimeType ?? '';
  if (response.statusCode != 200 || (wantHtml ? !kind.contains('html') : !kind.startsWith('image/'))) {
    await response.drain<void>();
    return null;
  }
  final out = BytesBuilder(copy: false);
  await for (final chunk in response) {
    out.add(chunk);
    // The head of a page is enough; a picture that large is not a preview.
    if (out.length > limit) {
      if (!wantHtml) return null;
      break;
    }
  }
  return out.takeBytes();
}

/// Fetches a page and makes its card. Null if there is nothing to show, the
/// page did not answer in time, or it is not a web page.
Future<LinkPreview?> fetchPreview(String url) async {
  if (!isWebAddress(url)) return null;
  final client = HttpClient()..connectionTimeout = const Duration(seconds: 4);
  try {
    return await () async {
      final page = await _fetch(client, Uri.parse(url), 300 * 1024, wantHtml: true);
      if (page == null) return null;
      final about = readPage(utf8.decode(page, allowMalformed: true));
      Uint8List? image;
      if (about.image.isNotEmpty) {
        final address = Uri.parse(url).resolve(about.image);
        if (address.scheme == 'http' || address.scheme == 'https') {
          try {
            final bytes = await _fetch(client, address, 3 * 1024 * 1024, wantHtml: false);
            if (bytes != null) image = await compute(shrinkPreviewImage, bytes);
          } catch (_) {
            // A card without a picture is still a card.
          }
        }
      }
      final preview = LinkPreview(url: url, title: about.title, description: about.description, image: image);
      return preview.isEmpty ? null : preview;
    }()
        .timeout(const Duration(seconds: 8));
  } catch (_) {
    return null;
  } finally {
    client.close(force: true);
  }
}
