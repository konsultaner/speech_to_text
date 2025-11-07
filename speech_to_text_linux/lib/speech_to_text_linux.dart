import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:speech_to_text_platform_interface/speech_to_text_platform_interface.dart';

/// Linux implementation for the `speech_to_text` plugin that bridges the
/// method-channel calls to the native Vosk + PortAudio backend.
class SpeechToTextLinux extends SpeechToTextPlatform {
  static const MethodChannel _channel = MethodChannel('speech_to_text_linux');
  static bool _handlerRegistered = false;

  /// Registers this class as the default instance of [SpeechToTextPlatform].
  static void registerWith() {
    SpeechToTextPlatform.instance = SpeechToTextLinux();
  }

  @override
  Future<bool> hasPermission() async {
    try {
      final bool? result = await _channel.invokeMethod<bool>('hasPermission');
      return result ?? false;
    } catch (error, stackTrace) {
      if (kDebugMode) {
        debugPrint('SpeechToTextLinux.hasPermission error: $error\n$stackTrace');
      }
      return false;
    }
  }

  @override
  Future<bool> initialize({
    debugLogging = false,
    List<SpeechConfigOption>? options,
  }) async {
    _ensureHandlerRegistered();
    final params = _mergeLinuxOptions(debugLogging, options);
    try {
      final bool? result =
          await _channel.invokeMethod<bool>('initialize', params);
      return result ?? false;
    } catch (error, stackTrace) {
      if (kDebugMode) {
        debugPrint('SpeechToTextLinux.initialize error: $error\n$stackTrace');
      }
      return false;
    }
  }

  @override
  Future<bool> listen({
    String? localeId,
    @Deprecated('Use SpeechListenOptions.partialResults instead')
    partialResults = true,
    @Deprecated('Use SpeechListenOptions.onDevice instead') onDevice = false,
    @Deprecated('Use SpeechListenOptions.listenMode instead')
    int listenMode = 0,
    @Deprecated('Use SpeechListenOptions.sampleRate instead') sampleRate = 0,
    SpeechListenOptions? options,
  }) async {
    final Map<String, dynamic> params = {
      'localeId': localeId,
      'partialResults': options?.partialResults ?? partialResults,
      'listenMode': options?.listenMode.index ?? listenMode,
      'sampleRate': options?.sampleRate ?? sampleRate,
      'onDevice': options?.onDevice ?? onDevice,
      'autoPunctuation': options?.autoPunctuation ?? false,
      'enableHapticFeedback': options?.enableHapticFeedback ?? false,
      'cancelOnError': options?.cancelOnError ?? false,
      'pauseForMillis': options?.pauseFor?.inMilliseconds,
      'listenForMillis': options?.listenFor?.inMilliseconds,
    };

    try {
      _ensureHandlerRegistered();
      final bool? result = await _channel.invokeMethod<bool>('listen', params);
      return result ?? false;
    } catch (error, stackTrace) {
      if (kDebugMode) {
        debugPrint('SpeechToTextLinux.listen error: $error\n$stackTrace');
      }
      return false;
    }
  }

  @override
  Future<void> stop() async {
    try {
      _ensureHandlerRegistered();
      await _channel.invokeMethod<void>('stop');
    } catch (error, stackTrace) {
      if (kDebugMode) {
        debugPrint('SpeechToTextLinux.stop error: $error\n$stackTrace');
      }
    }
  }

  @override
  Future<void> cancel() async {
    try {
      _ensureHandlerRegistered();
      await _channel.invokeMethod<void>('cancel');
    } catch (error, stackTrace) {
      if (kDebugMode) {
        debugPrint('SpeechToTextLinux.cancel error: $error\n$stackTrace');
      }
    }
  }

  @override
  Future<List<dynamic>> locales() async {
    try {
      _ensureHandlerRegistered();
      final List<dynamic>? result =
          await _channel.invokeMethod<List<dynamic>>('locales');
      return result ?? const [];
    } catch (error, stackTrace) {
      if (kDebugMode) {
        debugPrint('SpeechToTextLinux.locales error: $error\n$stackTrace');
      }
      return const [];
    }
  }

  Map<String, dynamic> _mergeLinuxOptions(
      bool debugLogging, List<SpeechConfigOption>? options) {
    final Map<String, dynamic> params = {
      'debugLogging': debugLogging,
    };

    if (options != null) {
      for (final option in options) {
        if (option.platform == 'linux') {
          params[option.name] = option.value;
        }
      }
    }

    return params;
  }

  Future<void> _handleMethodCall(MethodCall call) async {
    try {
      switch (call.method) {
        case 'textRecognition':
          final payload = call.arguments;
          if (payload is String && onTextRecognition != null) {
            onTextRecognition!(payload);
          }
          break;
        case 'notifyError':
          final error = call.arguments;
          if (error is String && onError != null) {
            onError!(error);
          }
          break;
        case 'notifyStatus':
          final status = call.arguments;
          if (status is String && onStatus != null) {
            onStatus!(status);
          }
          break;
        case 'soundLevelChange':
          final level = call.arguments;
          if (level is double && onSoundLevel != null) {
            onSoundLevel!(level);
          }
          break;
        default:
          if (kDebugMode) {
            debugPrint('SpeechToTextLinux: unhandled callback ${call.method}');
          }
      }
    } catch (error, stackTrace) {
      if (kDebugMode) {
        debugPrint(
            'SpeechToTextLinux._handleMethodCall error: $error\n$stackTrace');
      }
    }
  }

  void _ensureHandlerRegistered() {
    if (_handlerRegistered) {
      return;
    }
    _channel.setMethodCallHandler(_handleMethodCall);
    _handlerRegistered = true;
  }
}
