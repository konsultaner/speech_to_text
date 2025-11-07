# speech_to_text_example

Demonstrates how to use the speech_to_text plugin. This example requires 
that the plugin has been installed. It initializes speech recognition, 
listens for words and prints them.  

## Linux setup

The example can run on Linux using the new Vosk backend. Before launching
`flutter run -d linux` install the native dependencies and point the example
at your model and optional library paths via environment variables:

```bash
sudo apt install portaudio19-dev            # PortAudio headers/runtime
sudo cp path/to/libvosk.so /usr/local/lib && sudo ldconfig
export SPEECH_TO_TEXT_LINUX_MODEL=/absolute/path/to/vosk-model
# Optional overrides:
export SPEECH_TO_TEXT_LINUX_LIB=/absolute/path/to/libvosk.so
export SPEECH_TO_TEXT_LINUX_LOCALE=en-US
export SPEECH_TO_TEXT_LINUX_LABEL="English (Vosk)"
```

If `SPEECH_TO_TEXT_LINUX_MODEL` is missing the Linux build will fail to
initialize and the app will display the error returned by the plugin.

## Source

```dart
import 'package:flutter/material.dart';
import 'dart:async';

import 'package:speech_to_text/speech_to_text.dart';
import 'package:speech_to_text/speech_recognition_result.dart';
import 'package:speech_to_text/speech_recognition_error.dart';

void main() => runApp(MyApp());

class MyApp extends StatefulWidget {
  @override
  _MyAppState createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  bool _hasSpeech = false;
  String lastWords = "";
  String lastError = "";
  String lastStatus = "";
  final SpeechToText speech = SpeechToText();

  @override
  void initState() {
    super.initState();
    initSpeechState();
  }

  Future<void> initSpeechState() async {
    bool hasSpeech = await speech.initialize(onError: errorListener, onStatus: statusListener );

    if (!mounted) return;
    setState(() {
      _hasSpeech = hasSpeech;
    });
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(
          title: const Text('Speech to Text Example'),
        ),
        body: _hasSpeech
            ? Column(children: [
                Expanded(
                  child: Center(
                    child: Text('Speech recognition available'),
                  ),
                ),
                Expanded(
                  child: Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: <Widget>[
                      TextButton(
                        child: Text('Start'),
                        onPressed: startListening,
                      ),
                      TextButton(
                        child: Text('Stop'),
                        onPressed: stopListening,
                      ),
                      TextButton(
                        child: Text('Cancel'),
                        onPressed:cancelListening,
                      ),
                    ],
                  ),
                ),
                Expanded(
                  child: Column(
                    children: <Widget>[
                      Center(
                        child: Text('Recognized Words'),
                      ),
                      Center(
                        child: Text(lastWords),
                      ),
                    ],
                  ),
                ),
                Expanded(
                  child: Column(
                    children: <Widget>[
                      Center(
                        child: Text('Error'),
                      ),
                      Center(
                        child: Text(lastError),
                      ),
                    ],
                  ),
                ),
                Expanded(
                  child: Center(
                    child: speech.isListening ? Text("I'm listening...") : Text( 'Not listening' ),
                  ),
                ),
              ])
            : Center( child: Text('Speech recognition unavailable', style: TextStyle(fontSize: 20.0, fontWeight: FontWeight.bold))),
      ),
    );
  }

  void startListening() {
    lastWords = "";
    lastError = "";
    speech.listen(onResult: resultListener );
    setState(() {
      
    });
  }

  void stopListening() {
    speech.stop( );
    setState(() {
      
    });
  }

  void cancelListening() {
    speech.cancel( );
    setState(() {
      
    });
  }

  void resultListener(SpeechRecognitionResult result) {
    setState(() {
      lastWords = "${result.recognizedWords} - ${result.finalResult}";
    });
  }

  void errorListener(SpeechRecognitionError error ) {
    setState(() {
      lastError = "${error.errorMsg} - ${error.permanent}";
    });
  }
  void statusListener(String status ) {
    setState(() {
      lastStatus = "$status";
    });
  }
}
```
