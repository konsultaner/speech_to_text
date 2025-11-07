import 'package:flutter_test/flutter_test.dart';
import 'package:speech_to_text_linux/speech_to_text_linux.dart';
import 'package:speech_to_text_platform_interface/speech_to_text_platform_interface.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('registerWith replaces the default platform implementation', () {
    final originalInstance = SpeechToTextPlatform.instance;
    addTearDown(() {
      SpeechToTextPlatform.instance = originalInstance;
    });

    SpeechToTextLinux.registerWith();

    expect(SpeechToTextPlatform.instance, isA<SpeechToTextLinux>());
  });
}
