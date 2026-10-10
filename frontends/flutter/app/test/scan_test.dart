import 'package:corded_app/screens/scan.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a recovery code carries the username and the key, and reads back', () {
    final code = recoveryCode('andrew', 'ABCDE-FGHIJ-KLMNO');
    final found = parseRecoveryCode(code)!;
    expect(found.username, 'andrew');
    expect(found.key, 'ABCDE-FGHIJ-KLMNO');
  });

  test('anything else is not a recovery code', () {
    expect(parseRecoveryCode('corded://host:7443/?fp=x&invite=y'), isNull);
    expect(parseRecoveryCode('corded-recovery://andrew'), isNull);
    expect(parseRecoveryCode('corded-recovery:///key'), isNull);
    expect(parseRecoveryCode('hello'), isNull);
  });
}
