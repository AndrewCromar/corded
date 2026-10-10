import 'dart:convert';
import 'dart:typed_data';

import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';
import 'package:image_picker/image_picker.dart';
import 'package:url_launcher/url_launcher.dart';

import '../app_state.dart';
import 'chat.dart';
import 'crop.dart';
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

  static Uint8List? _pictureBytes(Profile p) {
    if (p.picture.isEmpty) return null;
    try {
      return base64Decode(p.picture);
    } catch (_) {
      return null;
    }
  }

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
                child: PresenceAvatar(name: name, radius: 48, status: status, picture: _pictureBytes(p)),
              ),
              const SizedBox(height: 12),
              Center(
                child: Row(mainAxisSize: MainAxisSize.min, children: [
                  Flexible(child: Text(name, style: theme.textTheme.headlineSmall)),
                  if (p.bot) const BotTag(),
                ]),
              ),
              if (p.bot)
                const Padding(
                  padding: EdgeInsets.only(top: 4),
                  child: Center(child: Text('This account says it is a program, not a person.')),
                ),
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
              // Tappable: they open the phone's mail app or dialler.
              if (p.email.isNotEmpty)
                row(Icons.mail_outline, 'Email',
                    _contact(context, p.email, Uri(scheme: 'mailto', path: p.email))),
              if (p.phone.isNotEmpty)
                row(Icons.phone_outlined, 'Phone',
                    _contact(context, p.phone, Uri(scheme: 'tel', path: p.phone.replaceAll(' ', '')))),
              if (p.bio.isNotEmpty)
                row(Icons.notes, 'About', LinkedText(p.bio, linkColor: theme.colorScheme.primary)),
              for (final link in p.links)
                () {
                  final parts = splitProfileLink(link);
                  return row(
                      Icons.link,
                      parts.label.isEmpty ? 'Link' : parts.label,
                      LinkedText(parts.url.contains('://') ? parts.url : 'https://${parts.url}',
                          linkColor: theme.colorScheme.primary));
                }(),
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
  final _email = TextEditingController();
  final _phone = TextEditingController();
  // Each link: what it is called, and where it leads.
  final _links = <(TextEditingController, TextEditingController)>[];
  bool _loaded = false;
  bool _bot = false;
  String _picture = ''; // base64, as it will be saved
  bool _pictureChanged = false;

  Future<void> _choosePicture() async {
    final messenger = ScaffoldMessenger.of(context);
    try {
      final file =
          await ImagePicker().pickImage(source: ImageSource.gallery, maxWidth: 1024, maxHeight: 1024);
      if (file == null) return;
      final bytes = await file.readAsBytes();
      if (!mounted) return;
      final small = await cropPicture(context, bytes);
      if (small == null) return;
      if (small.isEmpty) {
        messenger.showSnackBar(const SnackBar(content: Text('That file is not a picture Corded can read.')));
        return;
      }
      if (mounted) {
        setState(() {
          _picture = small;
          _pictureChanged = true;
        });
      }
    } catch (_) {
      messenger.showSnackBar(const SnackBar(content: Text('The picture could not be opened.')));
    }
  }

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
        _email.text = p.email;
        _phone.text = p.phone;
        for (final link in p.links) {
          final parts = splitProfileLink(link);
          _links.add((TextEditingController(text: parts.label), TextEditingController(text: parts.url)));
        }
        _picture = p.picture;
        _bot = p.bot;
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
              'email': _email.text.trim(),
              'phone': _phone.text.trim(),
              'bot': _bot,
              if (_pictureChanged) 'picture': _picture,
              'links': [
                for (final (label, url) in _links)
                  if (url.text.trim().isNotEmpty) joinProfileLink(label.text.trim(), url.text.trim())
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
              Center(
                child: PresenceAvatar(
                    name: _displayName.text.isEmpty ? widget.state.store.username : _displayName.text,
                    radius: 44,
                    picture: () {
                      if (_picture.isEmpty) return null;
                      try {
                        return base64Decode(_picture);
                      } catch (_) {
                        return null;
                      }
                    }()),
              ),
              Row(mainAxisAlignment: MainAxisAlignment.center, children: [
                TextButton.icon(
                    onPressed: _choosePicture,
                    icon: const Icon(Icons.photo_outlined),
                    label: Text(_picture.isEmpty ? 'Add a picture' : 'Change picture')),
                if (_picture.isNotEmpty)
                  TextButton(
                      onPressed: () => setState(() {
                            _picture = '';
                            _pictureChanged = true;
                          }),
                      child: const Text('Remove')),
              ]),
              const SizedBox(height: 8),
              field(_displayName, 'Display name', helper: 'Shown instead of your username', max: 40),
              field(_fullName, 'Full name', max: 80),
              field(_birthday, 'Birthday', helper: '2004-05-17, or 05-17 without the year'),
              field(_bio, 'About you', lines: 3, max: 500),
              field(_email, 'Email', helper: 'Only if you want people here to have it', max: 120),
              field(_phone, 'Phone number', max: 40),
              SwitchListTile(
                contentPadding: EdgeInsets.zero,
                title: const Text('This account is a bot'),
                subtitle: const Text('Shows a BOT tag beside the name, so people know it is a program'),
                value: _bot,
                onChanged: (v) => setState(() => _bot = v),
              ),
              Padding(
                padding: const EdgeInsets.only(bottom: 8),
                child: Text('Links', style: Theme.of(context).textTheme.titleSmall),
              ),
              for (final (i, (label, url)) in _links.indexed)
                Padding(
                  padding: const EdgeInsets.only(bottom: 10),
                  child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Expanded(
                      flex: 2,
                      child: TextField(
                          controller: label,
                          decoration: const InputDecoration(
                              labelText: 'Label',
                              hintText: 'GitHub',
                              border: OutlineInputBorder(),
                              isDense: true)),
                    ),
                    const SizedBox(width: 8),
                    Expanded(
                      flex: 3,
                      child: TextField(
                          controller: url,
                          autocorrect: false,
                          keyboardType: TextInputType.url,
                          decoration: const InputDecoration(
                              labelText: 'Address',
                              hintText: 'https://',
                              border: OutlineInputBorder(),
                              isDense: true)),
                    ),
                    IconButton(
                        tooltip: 'Remove this link',
                        icon: const Icon(Icons.remove_circle_outline),
                        onPressed: () => setState(() => _links.removeAt(i))),
                  ]),
                ),
              if (_links.length < 5)
                Align(
                  alignment: Alignment.centerLeft,
                  child: TextButton.icon(
                      onPressed: () =>
                          setState(() => _links.add((TextEditingController(), TextEditingController()))),
                      icon: const Icon(Icons.add),
                      label: Text(_links.isEmpty ? 'Add a link' : 'Add another link')),
                ),
              const SizedBox(height: 80),
            ]),
      floatingActionButton: FloatingActionButton.extended(
          onPressed: _save, icon: const Icon(Icons.check), label: const Text('Save')),
    );
  }
}

/// A profile link is kept as "label|address" (or just the address).
({String label, String url}) splitProfileLink(String link) {
  final bar = link.indexOf('|');
  if (bar < 0) return (label: '', url: link);
  return (label: link.substring(0, bar), url: link.substring(bar + 1));
}

String joinProfileLink(String label, String url) {
  final clean = label.replaceAll('|', ' ').trim();
  return clean.isEmpty ? url : '$clean|$url';
}

// An email address or phone number: shown as a link that opens the device's
// mail app or dialler, and can be selected to copy.
Widget _contact(BuildContext context, String shown, Uri opens) {
  final colour = Theme.of(context).colorScheme.primary;
  return InkWell(
    onTap: () async {
      final messenger = ScaffoldMessenger.of(context);
      var opened = false;
      try {
        opened = await launchUrl(opens);
      } catch (_) {
        // Reported below.
      }
      if (!opened) messenger.showSnackBar(SnackBar(content: Text('Nothing on this device opens $shown.')));
    },
    child: Text(shown,
        style: TextStyle(color: colour, decoration: TextDecoration.underline, decorationColor: colour)),
  );
}
