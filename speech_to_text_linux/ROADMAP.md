# speech_to_text_linux roadmap

This document tracks the plan for bringing first-class Linux support to the
`speech_to_text` federated plugin. It captures the backend choices, milestones,
and open questions so the implementation stays aligned with the broader PR
discussion.

## Goals
- Deliver a Linux implementation that plugs into the existing
  `SpeechToTextPlatform` API with no app-side changes beyond supplying a Vosk
  model path and system dependencies.
- Provide a maintainable audio/recognizer pipeline (PortAudio + Vosk) with the
  same method-channel semantics as Android/iOS/Windows (partial/final results,
  sound levels, status/error callbacks).
- Document clear setup instructions (deps, model download, options) so plugin
  adopters can enable Linux with minimal friction.

## Engines considered
| Engine             | Notes                                                                                   |
| ------------------ | --------------------------------------------------------------------------------------- |
| **Vosk** (default) | Offline, light-weight, permissive license, multiple languages, fast startup.            |
| Whisper            | Higher accuracy, heavier models, good future optional backend (maybe via whisper.cpp).  |
| Picovoice Leopard  | Commercial SDK; good reference for locale/model UX but license prevents bundling.       |
| Coqui STT          | Legacy Mozilla DeepSpeech fork; interesting benchmark, less active now.                 |

We start with Vosk because it is proven on Linux (see `alphacep/vosk-flutter`
and `vosk_flutter`) and keeps the plugin dependency-free from cloud services.

## Milestones
1. **Bootstrapping (done)**
   - Scaffold federated package, wire Dart `SpeechToTextLinux` shim, expose
     Linux-specific `SpeechConfigOption` helpers in the core API.
   - Implement native plugin shell, dynamic linking to `libvosk`, PortAudio
     capture loop, partial/final result forwarding, sound level updates.
2. **Docs & setup (done)**
   - README guidance for installing PortAudio, `libvosk`, and models.
   - Change logs and version bumps for both the main and Linux packages.
3. **Robustness & polish (up next)**
   - Improve error surfacing (recognizer creation failures, missing devices).
   - Add runtime checks for model/locale metadata, support multiple locales if
     apps bundle more than one model.
   - Expose lightweight integration tests or sample configuration loader in the
     example app.
4. **Stretch goals**
   - Optional Whisper backend guarded by config flag.
   - Model download helper / caching utility for CI & sample apps.
   - CI job that installs PortAudio, fetches a tiny Vosk model, builds and runs
     the example on Linux.

## References
- `alphacep/vosk-flutter` – baseline for binding the C API on Linux.
- `vosk_flutter` – community-oriented Dart API surface worth comparing.
- Picovoice Leopard & Whisper Flutter demos – good UX references for locale
  selection, asset management, and documentation.

Feel free to extend this roadmap as the implementation evolves or new backends
are added.
