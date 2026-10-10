import 'package:corded_app/screens/server.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a typed channel name is tidied', () {
    expect(tidyChannelName('Hosting Discussion'), 'hosting-discussion');
    expect(tidyChannelName('  #Off   Topic! '), 'off-topic');
    expect(tidyChannelName('dev_ops-2'), 'dev_ops-2');
    expect(tidyChannelName('--weird--'), 'weird');
  });
}
