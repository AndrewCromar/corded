import 'package:flutter/material.dart';

/// How a presence is shown: its colour and what to call it.
({Color color, String label}) presenceLook(String status) => switch (status) {
      'online' => (color: const Color(0xFF2E9E5B), label: 'Online'),
      'away' => (color: const Color(0xFFE0A526), label: 'Away'),
      'dnd' => (color: const Color(0xFFD64545), label: 'Do not disturb'),
      _ => (color: const Color(0xFF8A8F98), label: 'Offline'),
    };

/// The small coloured dot beside a person.
class PresenceDot extends StatelessWidget {
  const PresenceDot(this.status, {super.key, this.size = 12});
  final String status;
  final double size;

  @override
  Widget build(BuildContext context) {
    final look = presenceLook(status);
    final offline = status != 'online' && status != 'away' && status != 'dnd';
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        // Offline is a ring, so it does not read as a colour of its own.
        color: offline ? Theme.of(context).colorScheme.surface : look.color,
        border: Border.all(color: offline ? look.color : Theme.of(context).colorScheme.surface, width: 2),
      ),
    );
  }
}

/// An avatar with the person's presence at its corner.
class PresenceAvatar extends StatelessWidget {
  const PresenceAvatar({super.key, required this.name, required this.status});
  final String name;
  final String status;

  @override
  Widget build(BuildContext context) {
    return Stack(clipBehavior: Clip.none, children: [
      CircleAvatar(child: Text(name.isEmpty ? '?' : name[0].toUpperCase())),
      Positioned(right: -2, bottom: -2, child: PresenceDot(status, size: 14)),
    ]);
  }
}
