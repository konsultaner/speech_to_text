## 1.0.0-beta.1

* Initial prerelease containing the Linux implementation of `speech_to_text`.
* Streams microphone audio through PortAudio and performs offline recognition with Vosk.
* Supports partial/final results, locale discovery, configurable model path and optional
  overrides for the shared `libvosk` library.
