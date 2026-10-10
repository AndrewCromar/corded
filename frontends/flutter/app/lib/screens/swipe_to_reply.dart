import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Lets a message be pulled to the right to reply to it. The message follows
/// the finger, a reply arrow appears behind it, and letting go past the
/// halfway mark starts the reply.
class SwipeToReply extends StatefulWidget {
  const SwipeToReply({super.key, required this.child, required this.onReply, this.enabled = true});
  final Widget child;
  final VoidCallback onReply;
  final bool enabled;

  @override
  State<SwipeToReply> createState() => _SwipeToReplyState();
}

class _SwipeToReplyState extends State<SwipeToReply> {
  static const _trigger = 64.0, _limit = 96.0;
  double _dx = 0;
  bool _dragging = false, _armed = false;

  @override
  Widget build(BuildContext context) {
    if (!widget.enabled) return widget.child;
    final scheme = Theme.of(context).colorScheme;
    return GestureDetector(
      behavior: HitTestBehavior.translucent,
      onHorizontalDragStart: (_) => setState(() => _dragging = true),
      onHorizontalDragUpdate: (d) {
        final next = (_dx + d.delta.dx).clamp(0.0, _limit);
        final armed = next >= _trigger;
        // A tick the moment letting go would reply.
        if (armed && !_armed) HapticFeedback.selectionClick();
        setState(() {
          _dx = next;
          _armed = armed;
        });
      },
      onHorizontalDragEnd: (_) {
        final reply = _armed;
        setState(() {
          _dx = 0;
          _dragging = false;
          _armed = false;
        });
        if (reply) widget.onReply();
      },
      onHorizontalDragCancel: () => setState(() {
        _dx = 0;
        _dragging = false;
        _armed = false;
      }),
      child: Stack(alignment: Alignment.centerLeft, children: [
        Positioned(
          left: 16,
          child: Opacity(
            opacity: (_dx / _trigger).clamp(0.0, 1.0),
            child: CircleAvatar(
              radius: 16,
              backgroundColor: _armed ? scheme.primary : scheme.surfaceContainerHighest,
              child: Icon(Icons.reply, size: 18, color: _armed ? scheme.onPrimary : scheme.onSurfaceVariant),
            ),
          ),
        ),
        AnimatedContainer(
          duration: _dragging ? Duration.zero : const Duration(milliseconds: 160),
          curve: Curves.easeOut,
          transform: Matrix4.translationValues(_dx, 0, 0),
          child: widget.child,
        ),
      ]),
    );
  }
}
