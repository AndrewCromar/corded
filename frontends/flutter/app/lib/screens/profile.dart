import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'chat.dart';
import 'common.dart';
import 'linked_text.dart';
import 'presence.dart';

/// What someone has said about themselves, as far as this device knows.
class ProfileScreen extends StatefulWidget {
  const ProfileScreen({super.key, required this.state, required this.userId, required this.username});
  final AppState state;
  final String userId;
  final String username;

  @override
  State<ProfileScreen> createState() => _ProfileScreenState();
}

class _ProfileScreenState extends State<ProfileScreen> {
  Profile? _profile;

  bool get _me => widget.username == widget.state.store.username;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    try {
      final p = await widget.state.store.profile(_me ? null : widget.userId);
      if (mounted) setState(() => _profile = p);
    } on CordedError {
      if (mounted) setState(() => _profile = Profile.fromJson(const {}));
    }
  }

  Future<void> _message() async {
    final navigator = Navigator.of(context);
    await attempt(context, () async {
      final r = await widget.state.engine.command({
        'cmd': 'start_chat',
        'username': widget.username,
        if (widget.state.server != null) 'server_id': widget.state.server!.id,
      });
      final room = (r['room'] as Map?)?['room_id'] as String?;
      if (room != null) {
        navigator.push(MaterialPageRoute(builder: (_) => ChatScreen(state: widget.state, roomId: room)));
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final p = _profile;
    final server = widget.state.server;
    final status = server == null ? 'offline' : widget.state.store.presence(server.id, widget.userId);
    final name = (p?.displayName.isNotEmpty ?? false) ? p!.displayName : widget.username;
    Widget row(IconData icon, String label, Widget value) => ListTile(
          leading: Icon(icon),
          title: Text(label, style: theme.textTheme.labelMedium),
          subtitle: DefaultTextStyle.merge(style: theme.textTheme.bodyLarge, child: value),
        );
    return Scaffold(
      appBar: AppBar(
        title: const Text('Profile'),
        actions: [
          if (_me)
            IconButton(
              tooltip: 'Edit your profile',
              icon: const Icon(Icons.edit_outlined),
              onPressed: () async {
                await Navigator.push(
                    context, MaterialPageRoute(builder: (_) => EditProfileScreen(state: widget.state)));
                _load();
              },
            ),
        ],
      ),
      body: p == null
          ? const Center(child: CircularProgressIndicator())
          : ListView(children: [
              const SizedBox(height: 24),
              Center(
                child: Stack(clipBehavior: Clip.none, children: [
                  CircleAvatar(
                      radius: 44,
                      child: Text(name.isEmpty ? '?' : name[0].toUpperCase(),
                          style: theme.textTheme.displaySmall)),
                  Positioned(right: 0, bottom: 0, child: PresenceDot(status, size: 22)),
                ]),
              ),
              const SizedBox(height: 12),
              Center(child: Text(name, style: theme.textTheme.headlineSmall)),
              Center(
                  child: Text('@${widget.username}  ·  ${presenceLook(status).label}',
                      style: theme.textTheme.bodyMedium)),
              const SizedBox(height: 12),
              if (!_me)
                Center(
                  child: FilledButton.tonalIcon(
                      onPressed: _message,
                      icon: const Icon(Icons.chat_outlined),
                      label: const Text('Message')),
                ),
              const SizedBox(height: 8),
              if (p.fullName.isNotEmpty) row(Icons.person_outline, 'Name', Text(p.fullName)),
              if (p.birthday.isNotEmpty) row(Icons.cake_outlined, 'Birthday', Text(p.birthday)),
              if (p.bio.isNotEmpty)
                row(Icons.notes, 'About', LinkedText(p.bio, linkColor: theme.colorScheme.primary)),
              for (final link in p.links)
                row(Icons.link, 'Link', LinkedText(link, linkColor: theme.colorScheme.primary)),
              if (p.isEmpty)
                Padding(
                  padding: const EdgeInsets.all(24),
                  child: Text(
                      _me
                          ? 'You have not added anything yet. Tap the pencil to say who you are.'
                          : 'Nothing here yet. A profile arrives, encrypted, the next time this person '
                              'writes in a chat you share.',
                      textAlign: TextAlign.center),
                ),
              Padding(
                padding: const EdgeInsets.all(24),
                child: Text(
                    'Profiles are sent only to people who share a chat with their owner. No server can read them.',
                    textAlign: TextAlign.center,
                    style: theme.textTheme.bodySmall),
              ),
            ]),
    );
  }
}

/// Your own profile, for editing. Every field is optional.
class EditProfileScreen extends StatefulWidget {
  const EditProfileScreen({super.key, required this.state});
  final AppState state;

  @override
  State<EditProfileScreen> createState() => _EditProfileScreenState();
}

class _EditProfileScreenState extends State<EditProfileScreen> {
  final _displayName = TextEditingController();
  final _fullName = TextEditingController();
  final _birthday = TextEditingController();
  final _bio = TextEditingController();
  final _links = TextEditingController();
  bool _loaded = false;

  @override
  void initState() {
    super.initState();
    widget.state.store.profile().then((p) {
      if (!mounted) return;
      setState(() {
        _displayName.text = p.displayName;
        _fullName.text = p.fullName;
        _birthday.text = p.birthday;
        _bio.text = p.bio;
        _links.text = p.links.join('\n');
        _loaded = true;
      });
    }).catchError((_) {
      if (mounted) setState(() => _loaded = true);
    });
  }

  Future<void> _save() async {
    final navigator = Navigator.of(context);
    final birthday = _birthday.text.trim();
    if (birthday.isNotEmpty &&
        !RegExp(r'^\d{4}-\d{2}-\d{2}$').hasMatch(birthday) &&
        !RegExp(r'^\d{2}-\d{2}$').hasMatch(birthday)) {
      ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Write the birthday as 2004-05-17, or 05-17 to leave out the year.')));
      return;
    }
    final ok = await attempt(
        context,
        () => widget.state.engine.command({
              'cmd': 'set_profile',
              'display_name': _displayName.text.trim(),
              'full_name': _fullName.text.trim(),
              'birthday': birthday,
              'bio': _bio.text.trim(),
              'links': [
                for (final line in _links.text.split('\n'))
                  if (line.trim().isNotEmpty) line.trim()
              ],
            }));
    if (ok) {
      // The display name now lives in the profile; clear any the servers held.
      for (final server in widget.state.store.servers.values) {
        widget.state.engine
            .command({'cmd': 'set_nickname', 'nickname': '', 'server_id': server.id}).catchError(
                (_) => <String, dynamic>{});
      }
      navigator.pop();
    }
  }

  @override
  Widget build(BuildContext context) {
    Widget field(TextEditingController c, String label, {String? helper, int lines = 1, int? max}) => Padding(
          padding: const EdgeInsets.only(bottom: 14),
          child: TextField(
            controller: c,
            minLines: lines,
            maxLines: lines == 1 ? 1 : lines + 3,
            maxLength: max,
            decoration:
                InputDecoration(labelText: label, helperText: helper, border: const OutlineInputBorder()),
          ),
        );
    return Scaffold(
      appBar: AppBar(title: const Text('Your profile')),
      body: !_loaded
          ? const Center(child: CircularProgressIndicator())
          : ListView(padding: const EdgeInsets.all(16), children: [
              const Padding(
                padding: EdgeInsets.only(bottom: 16),
                child:
                    Text('Everything here is optional. It is sent, encrypted, to the people you share a chat '
                        'with. Servers only ever see your username.'),
              ),
              field(_displayName, 'Display name', helper: 'Shown instead of your username', max: 40),
              field(_fullName, 'Full name', max: 80),
              field(_birthday, 'Birthday', helper: '2004-05-17, or 05-17 without the year'),
              field(_bio, 'About you', lines: 3, max: 500),
              field(_links, 'Links', helper: 'One per line, up to five', lines: 2),
              const SizedBox(height: 80),
            ]),
      floatingActionButton: FloatingActionButton.extended(
          onPressed: _save, icon: const Icon(Icons.check), label: const Text('Save')),
    );
  }
}
