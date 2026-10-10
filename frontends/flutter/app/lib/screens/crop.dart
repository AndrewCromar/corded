import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../picture.dart';

/// Lets the person move and zoom a chosen picture inside the round frame it
/// will be shown in. Gives back the small picture as base64, an empty string
/// if the file is not a picture, or null if they backed out.
Future<String?> cropPicture(BuildContext context, Uint8List bytes) async {
  final ui.Image shape;
  try {
    shape = await decodeImageFromList(bytes);
  } catch (_) {
    return '';
  }
  if (!context.mounted) return null;
  return Navigator.push<String>(
      context,
      MaterialPageRoute(
          builder: (_) =>
              _CropScreen(bytes: bytes, width: shape.width.toDouble(), height: shape.height.toDouble())));
}

class _CropScreen extends StatefulWidget {
  const _CropScreen({required this.bytes, required this.width, required this.height});
  final Uint8List bytes;
  final double width, height;

  @override
  State<_CropScreen> createState() => _CropScreenState();
}

class _CropScreenState extends State<_CropScreen> {
  final _view = TransformationController();
  bool _working = false;
  bool _placed = false;

  // Starts with the middle of the picture in the frame.
  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (_placed) return;
    _placed = true;
    final frame = MediaQuery.sizeOf(context).shortestSide - 48;
    final shorter = widget.width < widget.height ? widget.width : widget.height;
    _view.value = Matrix4.translationValues(
        -(frame * widget.width / shorter - frame) / 2, -(frame * widget.height / shorter - frame) / 2, 0);
  }

  @override
  void dispose() {
    _view.dispose();
    super.dispose();
  }

  // The picture is laid out so its shorter edge fills the frame; from the
  // view's zoom and position comes the square of the picture inside the frame.
  Future<void> _use(double frame) async {
    setState(() => _working = true);
    final scale = _view.value.getMaxScaleOnAxis();
    final shift = _view.value.getTranslation();
    final shorter = widget.width < widget.height ? widget.width : widget.height;
    final laidWidth = frame * widget.width / shorter, laidHeight = frame * widget.height / shorter;
    final small = await compute(cropToProfilePicture, (
      bytes: widget.bytes,
      left: (-shift.x / scale) / laidWidth,
      top: (-shift.y / scale) / laidHeight,
      side: (frame / scale) / laidWidth,
    ));
    if (mounted) Navigator.pop(context, small ?? '');
  }

  @override
  Widget build(BuildContext context) {
    final frame = MediaQuery.sizeOf(context).shortestSide - 48;
    final shorter = widget.width < widget.height ? widget.width : widget.height;
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        backgroundColor: Colors.black,
        foregroundColor: Colors.white,
        title: const Text('Move and zoom'),
        actions: [
          TextButton(
            onPressed: _working ? null : () => _use(frame),
            child: _working
                ? const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                : const Text('Use'),
          ),
        ],
      ),
      body: Center(
        child: SizedBox(
          width: frame,
          height: frame,
          child: Stack(fit: StackFit.expand, children: [
            ClipRect(
              child: InteractiveViewer(
                transformationController: _view,
                constrained: false,
                minScale: 1,
                maxScale: 6,
                child: SizedBox(
                  width: frame * widget.width / shorter,
                  height: frame * widget.height / shorter,
                  child: Image.memory(widget.bytes, fit: BoxFit.fill, gaplessPlayback: true),
                ),
              ),
            ),
            // What falls outside the circle is dimmed: that is how it will look.
            IgnorePointer(
              child: ClipPath(
                clipper: _OutsideCircle(),
                child: Container(color: Colors.black.withValues(alpha: 0.55)),
              ),
            ),
          ]),
        ),
      ),
    );
  }
}

class _OutsideCircle extends CustomClipper<Path> {
  @override
  Path getClip(Size size) => Path()
    ..fillType = PathFillType.evenOdd
    ..addRect(Offset.zero & size)
    ..addOval(Offset.zero & size);

  @override
  bool shouldReclip(covariant CustomClipper<Path> oldClipper) => false;
}
