import 'package:corded_dart/corded_dart.dart';
import 'package:flutter/material.dart';

import '../app_state.dart';
import 'common.dart';

/// The devices signed in as this person on the current server.
class DevicesScreen extends StatefulWidget {
  const DevicesScreen({super.key, required this.state});
  final AppState state;

  @override
  State<DevicesScreen> createState() => _DevicesScreenState();
}

class _DevicesScreenState extends State<DevicesScreen> {
  List<Map<String, dynamic>>? _devices;
  String? _error;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    try {
      final r =
          await widget.state.engine.command({'cmd': 'list_devices', 'server_id': widget.state.server!.id});
      final list = [for (final d in (r['devices'] as List? ?? const [])) (d as Map).cast<String, dynamic>()];
      if (mounted) setState(() => _devices = list);
    } on CordedError catch (e) {
      if (mounted) setState(() => _error = sentence(e.message));
    }
  }

  static String _when(int ms) {
    if (ms == 0) return 'unknown date';
    final t = DateTime.fromMillisecondsSinceEpoch(ms);
    String two(int n) => n.toString().padLeft(2, '0');
    return '${t.year}-${two(t.month)}-${two(t.day)} ${two(t.hour)}:${two(t.minute)}';
  }

  // A short, readable form of a device's id, to tell devices apart.
  static String _short(String id) => id.length < 8 ? id : '${id.substring(0, 4)}-${id.substring(4, 8)}';

  Future<void> _remove(Map<String, dynamic> device) async {
    final go = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Sign this device out?'),
        content: Text(
            'Device ${_short('${device['device_id']}')}, added ${_when((device['added_at'] as num?)?.toInt() ?? 0)}.\n\n'
            'It stops receiving your messages on this server straight away. The messages already on it stay on it. '
            'If you did not add this device yourself, someone has your recovery key: you cannot change that key, '
            'so tell the people you talk to.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Sign it out')),
        ],
      ),
    );
    if (go != true || !mounted) return;
    await attempt(
        context,
        () => widget.state.engine.command({
              'cmd': 'remove_device',
              'server_id': widget.state.server!.id,
              'device_id': device['device_id']
            }));
    await _load();
  }

  @override
  Widget build(BuildContext context) {
    final devices = _devices;
    return Scaffold(
      appBar: AppBar(title: const Text('Your devices')),
      body: _error != null
          ? Center(
              child: Padding(
                  padding: const EdgeInsets.all(24), child: Text(_error!, textAlign: TextAlign.center)))
          : devices == null
              ? const Center(child: CircularProgressIndicator())
              : ListView(children: [
                  Padding(
                    padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
                    child: Text('Devices signed in as ${widget.state.store.username} on '
                        '${widget.state.server?.name ?? 'this server'}. Each was set up with your recovery key. '
                        'If you see one you do not recognise, sign it out.'),
                  ),
                  for (final d in devices)
                    ListTile(
                      leading: Icon(d['this_device'] == true ? Icons.smartphone : Icons.devices_other),
                      title: Text(
                          d['this_device'] == true ? 'This device' : 'Device ${_short('${d['device_id']}')}'),
                      subtitle: Text('Added ${_when((d['added_at'] as num?)?.toInt() ?? 0)}'),
                      trailing: d['this_device'] == true
                          ? null
                          : TextButton(onPressed: () => _remove(d), child: const Text('Sign out')),
                    ),
                ]),
    );
  }
}
