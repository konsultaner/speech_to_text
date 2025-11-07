# speech_to_text_linux

Linux implementation of the [`speech_to_text`](../speech_to_text) plugin. Speech
recognition runs fully on-device using the [Vosk](https://alphacephei.com/vosk/)
engine with microphone capture provided by [PortAudio](http://www.portaudio.com/).
The package plugs into the federated architecture so apps only need to depend on
`speech_to_text` directly.

> ⚠️ This implementation is currently released as a beta. Accuracy and
> performance depend on the Vosk model you choose and the hardware it runs on.

## Requirements

1. **PortAudio development files** – used for low-latency microphone capture.
   ```bash
   sudo apt install portaudio19-dev
   ```

2. **libvosk** – shared library that exposes the Vosk C API.
   ```bash
   # Download the latest release and unpack it into a directory on your PATH.
   # https://github.com/alphacep/vosk-api/releases
   sudo cp libvosk.so /usr/local/lib/
   sudo ldconfig
   ```
   If you cannot install globally, pass the absolute path to the shared library
   via `SpeechConfigOption('linux', 'voskLibraryPath', '/custom/libvosk.so')`.

3. **Vosk model files** – each application must ship or download the acoustic
   model it needs (for example `vosk-model-small-en-us-0.15`). Keep the model on
   disk and provide its folder when initializing.

None of the above assets are bundled in this repository so that applications can
choose the languages, footprints, and licensing terms that best suit them.

## Usage

From your application call `SpeechToText.initialize` with a Linux-specific
`SpeechConfigOption` pointing at the Vosk model path:

```dart
final speech = SpeechToText();
final available = await speech.initialize(
  onError: _handleError,
  onStatus: _handleStatus,
  options: [
    SpeechToText.linuxModelPath('/opt/models/vosk-model-small-en-us-0.15'),
    // Optional: SpeechToText.linuxVoskLibrary('/opt/vosk/libvosk.so'),
    // Optional locale metadata displayed by `locales()`
    const SpeechConfigOption('linux', 'modelLocale', 'en-US'),
    const SpeechConfigOption('linux', 'modelDisplayName', 'English (Vosk)'),
  ],
);
```

Once initialized, all `listen`, `stop`, `cancel`, and `locales` APIs behave the
same as on the other platforms. Partial and final results are delivered through
the existing callbacks, and sound-level updates mirror Android/iOS semantics.

### Supported Linux options

| Option name        | Description                                                                 |
| ------------------ | --------------------------------------------------------------------------- |
| `modelPath`        | **Required.** Absolute path to the unpacked Vosk model directory.            |
| `voskLibraryPath`  | Optional override for the location of `libvosk.so` if it is not on `LD_LIBRARY_PATH`. |
| `modelLocale`      | Optional BCP-47 tag returned from `locales()`.                               |
| `modelDisplayName` | Optional label paired with `modelLocale` (defaults to `<locale> (Vosk)`).    |

Additional `SpeechListenOptions` such as `listenFor`, `pauseFor`, and
`partialResults` are also respected on Linux.

## Example project

The bundled [example](example/) app is a standard Flutter desktop target. Add
your own Vosk model path inside the `initialize` call to run it locally.
