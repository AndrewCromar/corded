import 'package:flutter/material.dart';
import 'package:mobile_scanner/mobile_scanner.dart';
import 'package:qr_flutter/qr_flutter.dart';

/// A code for another phone to scan. Always dark on white so it reads in
/// either theme.
class CodeToScan extends StatelessWidget {
  const CodeToScan(this.data, {super.key, this.size = 220});
  final String data;
  final double size;

  @override
  Widget build(BuildContext context) {
    return Container(
      color: Colors.white,
      padding: const EdgeInsets.all(8),
      child: QrImageView(data: data, size: size, backgroundColor: Colors.white),
    );
  }
}

/// Opens the camera and returns the text of the first code it sees, or null
/// if the person goes back.
Future<String?> scanCode(BuildContext context, {String title = 'Scan a code'}) =>
    Navigator.push<String>(context, MaterialPageRoute(builder: (_) => _ScanScreen(title: title)));

class _ScanScreen extends StatefulWidget {
  const _ScanScreen({required this.title});
  final String title;

  @override
  State<_ScanScreen> createState() => _ScanScreenState();
}

class _ScanScreenState extends State<_ScanScreen> {
  bool _done = false;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text(widget.title)),
      body: Stack(alignment: Alignment.bottomCenter, children: [
        MobileScanner(
          onDetect: (capture) {
            if (_done) return;
            for (final code in capture.barcodes) {
              final text = code.rawValue;
              if (text != null && text.isNotEmpty) {
                _done = true;
                Navigator.pop(context, text);
                return;
              }
            }
          },
          errorBuilder: (context, error, child) => Center(
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: Text(
                  'The camera could not be used. Allow Corded to use the camera in Android\'s settings, '
                  'or type the code instead.',
                  textAlign: TextAlign.center,
                  style: Theme.of(context).textTheme.bodyLarge),
            ),
          ),
        ),
        const Padding(
          padding: EdgeInsets.all(24),
          child: Material(
            color: Colors.black54,
            borderRadius: BorderRadius.all(Radius.circular(12)),
            child: Padding(
              padding: EdgeInsets.symmetric(horizontal: 16, vertical: 10),
              child: Text('Point the camera at the code', style: TextStyle(color: Colors.white)),
            ),
          ),
        ),
      ]),
    );
  }
}

/// What a recovery code holds: who, and their recovery key.
const recoveryScheme = 'corded-recovery://';

String recoveryCode(String username, String key) => '$recoveryScheme$username/$key';

({String username, String key})? parseRecoveryCode(String text) {
  if (!text.startsWith(recoveryScheme)) return null;
  final rest = text.substring(recoveryScheme.length);
  final slash = rest.indexOf('/');
  if (slash <= 0 || slash == rest.length - 1) return null;
  return (username: rest.substring(0, slash), key: rest.substring(slash + 1));
}
