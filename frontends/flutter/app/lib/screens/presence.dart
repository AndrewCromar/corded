import 'dart:typed_data';

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

/// A person's picture, or their initial when they have none, with their
/// presence at its corner when [status] is given.
class PresenceAvatar extends StatelessWidget {
  const PresenceAvatar({super.key, required this.name, this.status, this.picture, this.radius = 20});
  final String name;
  final String? status;
  final Uint8List? picture;
  final double radius;

  @override
  Widget build(BuildContext context) {
    final avatar = CircleAvatar(
      radius: radius,
      backgroundImage: picture == null ? null : MemoryImage(picture!),
      child: picture != null
          ? null
          : Text(name.isEmpty ? '?' : name[0].toUpperCase(), style: TextStyle(fontSize: radius * 0.9)),
    );
    if (status == null) return avatar;
    return Stack(clipBehavior: Clip.none, children: [
      avatar,
      Positioned(right: -2, bottom: -2, child: PresenceDot(status!, size: radius * 0.7)),
    ]);
  }
}

/// Marks a program, so people know they are not talking to a person.
class BotTag extends StatelessWidget {
  const BotTag({super.key});

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Container(
      margin: const EdgeInsets.only(left: 6),
      padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1),
      decoration: BoxDecoration(color: scheme.secondaryContainer, borderRadius: BorderRadius.circular(5)),
      child: Text('BOT',
          style: Theme.of(context).textTheme.labelSmall?.copyWith(
              color: scheme.onSecondaryContainer, fontWeight: FontWeight.bold, letterSpacing: 0.5)),
    );
  }
}
