import 'dart:io';

import 'package:speech_to_text/speech_to_text.dart';

const _modelEnvKey = 'SPEECH_TO_TEXT_LINUX_MODEL';
const _libraryEnvKey = 'SPEECH_TO_TEXT_LINUX_LIB';
const _localeEnvKey = 'SPEECH_TO_TEXT_LINUX_LOCALE';
const _labelEnvKey = 'SPEECH_TO_TEXT_LINUX_LABEL';

List<SpeechConfigOption> resolvePlatformInitializeOptions() {
  if (!Platform.isLinux) {
    return const [];
  }

  final modelPath = Platform.environment[_modelEnvKey];
  if (modelPath == null || modelPath.isEmpty) {
    return const [];
  }

  final options = <SpeechConfigOption>[
    SpeechToText.linuxModelPath(modelPath),
  ];

  final voskLibrary = Platform.environment[_libraryEnvKey];
  if (voskLibrary != null && voskLibrary.isNotEmpty) {
    options.add(SpeechToText.linuxVoskLibrary(voskLibrary));
  }

  final locale = Platform.environment[_localeEnvKey];
  if (locale != null && locale.isNotEmpty) {
    options.add(SpeechConfigOption('linux', 'modelLocale', locale));
  }

  final label = Platform.environment[_labelEnvKey];
  if (label != null && label.isNotEmpty) {
    options.add(SpeechConfigOption('linux', 'modelDisplayName', label));
  }

  return options;
}
