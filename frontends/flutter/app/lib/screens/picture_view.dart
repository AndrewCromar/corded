import 'dart:io';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';

/// One photo, full size. Fetched and decrypted when first opened, then kept
/// on this device. Pinch to zoom.
class PictureScreen extends StatefulWidget {
  const PictureScreen({super.key, required this.state, required this.message});
  final AppState state;
  final Message message;

  @override
  State<PictureScreen> createState() => _PictureScreenState();
}

class _PictureScreenState extends State<PictureScreen> {
  late final Future<String> _path = _fetch();

  Future<String> _fetch() async {
    final r = await widget.state.engine
        .command({'cmd': 'download_file', 'room_id': widget.message.roomId, 'event_id': widget.message.id});
    return r['path'] as String;
  }

  @override
  Widget build(BuildContext context) {
    final m = widget.message;
    final preview = m.thumbnail;
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        backgroundColor: Colors.black,
        foregroundColor: Colors.white,
        title: Text(m.fileName, maxLines: 1, overflow: TextOverflow.ellipsis),
      ),
      body: FutureBuilder<String>(
        future: _path,
        builder: (context, snapshot) {
          if (snapshot.hasError) {
            final error = snapshot.error;
            return Center(
              child: Padding(
                padding: const EdgeInsets.all(24),
                child: Text(error is CordedError ? error.message : 'The photo could not be opened.',
                    textAlign: TextAlign.center, style: const TextStyle(color: Colors.white)),
              ),
            );
          }
          if (!snapshot.hasData) {
            // The small preview stands in while the real one arrives.
            return Stack(fit: StackFit.expand, children: [
              if (preview != null) Image.memory(preview, fit: BoxFit.contain, gaplessPlayback: true),
              const Center(child: CircularProgressIndicator()),
            ]);
          }
          return InteractiveViewer(
            maxScale: 6,
            child: Center(child: Image.file(File(snapshot.data!), fit: BoxFit.contain)),
          );
        },
      ),
    );
  }
}
