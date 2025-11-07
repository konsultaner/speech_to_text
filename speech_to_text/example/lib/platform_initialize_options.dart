import 'package:speech_to_text/speech_to_text.dart';

import 'platform_initialize_options_stub.dart'
    if (dart.library.io) 'platform_initialize_options_io.dart';

/// Returns platform-specific initialization options for the example app.
List<SpeechConfigOption> platformInitializeOptions() =>
    resolvePlatformInitializeOptions();
