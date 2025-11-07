#include "include/speech_to_text_linux/speech_to_text_linux_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <portaudio.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <dlfcn.h>
#include <future>
#include <glib.h>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#define SPEECH_TO_TEXT_LINUX_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), speech_to_text_linux_plugin_get_type(), \
                              SpeechToTextLinuxPlugin))

namespace {

constexpr int kPartialResult = 0;
constexpr int kFinalResult = 2;

struct VoskModel;
struct VoskRecognizer;

struct StreamOpenResult {
  PaError error;
  PaStream* stream;
  bool timed_out;
};

class VoskApi {
 public:
  bool Load(const std::string& custom_path);
  void Unload();
  bool Ready() const { return handle_ != nullptr; }
  std::string last_error() const { return last_error_; }

  VoskModel* NewModel(const std::string& path) const;
  void FreeModel(VoskModel* model) const;

  VoskRecognizer* NewRecognizer(VoskModel* model, float sample_rate) const;
  void FreeRecognizer(VoskRecognizer* recognizer) const;
  int AcceptWaveform(VoskRecognizer* recognizer, const int16_t* data, int frames) const;
  std::string Result(VoskRecognizer* recognizer) const;
  std::string PartialResult(VoskRecognizer* recognizer) const;
  std::string FinalResult(VoskRecognizer* recognizer) const;
  void Reset(VoskRecognizer* recognizer) const;
  void EnableWordTimings(VoskRecognizer* recognizer) const;
  void EnablePartialWords(VoskRecognizer* recognizer, bool enabled) const;
  void ConfigureLogging(bool debug) const;

 private:
  void* handle_ = nullptr;
  mutable std::string last_error_;

  using ModelNewFn = VoskModel* (*)(const char*);
  using ModelFreeFn = void (*)(VoskModel*);
  using RecognizerNewFn = VoskRecognizer* (*)(VoskModel*, float);
  using RecognizerFreeFn = void (*)(VoskRecognizer*);
  using RecognizerAcceptFn = int (*)(VoskRecognizer*, const char*, int);
  using RecognizerResultFn = const char* (*)(VoskRecognizer*);
  using RecognizerResetFn = void (*)(VoskRecognizer*);
  using RecognizerSetIntFn = void (*)(VoskRecognizer*, int);
  using SetLogLevelFn = void (*)(int);

  ModelNewFn model_new_ = nullptr;
  ModelFreeFn model_free_ = nullptr;
  RecognizerNewFn recognizer_new_ = nullptr;
  RecognizerFreeFn recognizer_free_ = nullptr;
  RecognizerAcceptFn recognizer_accept_ = nullptr;
  RecognizerResultFn recognizer_result_ = nullptr;
  RecognizerResultFn recognizer_partial_ = nullptr;
  RecognizerResultFn recognizer_final_ = nullptr;
  RecognizerResetFn recognizer_reset_ = nullptr;
  RecognizerSetIntFn recognizer_set_words_ = nullptr;
  RecognizerSetIntFn recognizer_set_partial_words_ = nullptr;
  SetLogLevelFn set_log_level_ = nullptr;
};

class SpeechToTextLinuxPluginState {
 public:
  SpeechToTextLinuxPluginState() = default;
  ~SpeechToTextLinuxPluginState();

  void ResetRecognitionTimers();
  void JoinCaptureThread();

  std::mutex mutex;
  bool debug_logging = false;
  bool initialized = false;
  bool listening = false;
  bool partial_results_enabled = true;
  bool pa_initialized = false;

  int sample_rate = 16000;
  unsigned long frames_per_buffer = 1024;
  std::string model_path;
  std::string locale_tag = "en-US";
  std::string locale_label = "en-US:en-US (Vosk)";
  std::string last_partial_text;

  std::chrono::milliseconds listen_timeout{0};
  std::chrono::milliseconds pause_timeout{0};
  std::chrono::steady_clock::time_point listen_started;
  std::chrono::steady_clock::time_point last_speech_at;
  bool reported_speech = false;

  PaStream* stream = nullptr;
  VoskModel* model = nullptr;
  VoskRecognizer* recognizer = nullptr;
  VoskApi vosk;

  std::atomic<bool> stop_requested{false};
  std::atomic<bool> cancel_requested{false};

  std::thread capture_thread;
  bool capture_thread_running = false;
};

SpeechToTextLinuxPluginState::~SpeechToTextLinuxPluginState() {
  stop_requested.store(true);
  JoinCaptureThread();
  if (stream != nullptr) {
    Pa_CloseStream(stream);
    stream = nullptr;
  }
  if (recognizer != nullptr && vosk.Ready()) {
    vosk.FreeRecognizer(recognizer);
    recognizer = nullptr;
  }
  if (model != nullptr && vosk.Ready()) {
    vosk.FreeModel(model);
    model = nullptr;
  }
  if (pa_initialized) {
    Pa_Terminate();
    pa_initialized = false;
  }
  vosk.Unload();
}

void SpeechToTextLinuxPluginState::JoinCaptureThread() {
  if (capture_thread_running && capture_thread.joinable()) {
    capture_thread.join();
  }
  capture_thread_running = false;
}

void SpeechToTextLinuxPluginState::ResetRecognitionTimers() {
  listen_started = std::chrono::steady_clock::now();
  last_speech_at = listen_started;
  reported_speech = false;
  last_partial_text.clear();
  stop_requested.store(false);
  cancel_requested.store(false);
}

bool VoskApi::Load(const std::string& custom_path) {
  if (handle_ != nullptr) {
    return true;
  }
  std::vector<std::string> candidates;
  if (!custom_path.empty()) {
    candidates.push_back(custom_path);
  }
  candidates.emplace_back("libvosk.so");
  candidates.emplace_back("libvosk.so.1");

  for (const auto& candidate : candidates) {
    handle_ = dlopen(candidate.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle_ != nullptr) {
      break;
    }
  }

  if (handle_ == nullptr) {
    const char* error = dlerror();
    last_error_ = error != nullptr ? error : "Unable to load libvosk";
    return false;
  }

#define LOAD_VOSK_SYMBOL(field, symbol)                                              \
  field = reinterpret_cast<decltype(field)>(dlsym(handle_, symbol));                \
  if (field == nullptr) {                                                            \
    last_error_ = std::string("Missing symbol from libvosk: ") + symbol;          \
    Unload();                                                                        \
    return false;                                                                    \
  }

  LOAD_VOSK_SYMBOL(model_new_, "vosk_model_new");
  LOAD_VOSK_SYMBOL(model_free_, "vosk_model_free");
  LOAD_VOSK_SYMBOL(recognizer_new_, "vosk_recognizer_new");
  LOAD_VOSK_SYMBOL(recognizer_free_, "vosk_recognizer_free");
  LOAD_VOSK_SYMBOL(recognizer_accept_, "vosk_recognizer_accept_waveform");
  LOAD_VOSK_SYMBOL(recognizer_result_, "vosk_recognizer_result");
  LOAD_VOSK_SYMBOL(recognizer_partial_, "vosk_recognizer_partial_result");
  LOAD_VOSK_SYMBOL(recognizer_final_, "vosk_recognizer_final_result");
  LOAD_VOSK_SYMBOL(recognizer_reset_, "vosk_recognizer_reset");
  LOAD_VOSK_SYMBOL(recognizer_set_words_, "vosk_recognizer_set_words");
  LOAD_VOSK_SYMBOL(recognizer_set_partial_words_, "vosk_recognizer_set_partial_words");
  LOAD_VOSK_SYMBOL(set_log_level_, "vosk_set_log_level");

#undef LOAD_VOSK_SYMBOL

  last_error_.clear();
  return true;
}

void VoskApi::Unload() {
  if (handle_ != nullptr) {
    dlclose(handle_);
    handle_ = nullptr;
  }
  model_new_ = nullptr;
  model_free_ = nullptr;
  recognizer_new_ = nullptr;
  recognizer_free_ = nullptr;
  recognizer_accept_ = nullptr;
  recognizer_result_ = nullptr;
  recognizer_partial_ = nullptr;
  recognizer_final_ = nullptr;
  recognizer_reset_ = nullptr;
  recognizer_set_words_ = nullptr;
  recognizer_set_partial_words_ = nullptr;
  set_log_level_ = nullptr;
}

VoskModel* VoskApi::NewModel(const std::string& path) const {
  if (!Ready()) {
    return nullptr;
  }
  return model_new_ != nullptr ? model_new_(path.c_str()) : nullptr;
}

void VoskApi::FreeModel(VoskModel* model) const {
  if (model != nullptr && model_free_ != nullptr) {
    model_free_(model);
  }
}

VoskRecognizer* VoskApi::NewRecognizer(VoskModel* model, float sample_rate) const {
  if (!Ready() || model == nullptr || recognizer_new_ == nullptr) {
    return nullptr;
  }
  return recognizer_new_(model, sample_rate);
}

void VoskApi::FreeRecognizer(VoskRecognizer* recognizer) const {
  if (recognizer != nullptr && recognizer_free_ != nullptr) {
    recognizer_free_(recognizer);
  }
}

int VoskApi::AcceptWaveform(VoskRecognizer* recognizer, const int16_t* data, int frames) const {
  if (recognizer == nullptr || recognizer_accept_ == nullptr || data == nullptr || frames <= 0) {
    return 0;
  }
  const int bytes = static_cast<int>(frames * sizeof(int16_t));
  return recognizer_accept_(recognizer, reinterpret_cast<const char*>(data), bytes);
}

std::string VoskApi::Result(VoskRecognizer* recognizer) const {
  if (recognizer == nullptr || recognizer_result_ == nullptr) {
    return {};
  }
  const char* value = recognizer_result_(recognizer);
  return value != nullptr ? std::string(value) : std::string();
}

std::string VoskApi::PartialResult(VoskRecognizer* recognizer) const {
  if (recognizer == nullptr || recognizer_partial_ == nullptr) {
    return {};
  }
  const char* value = recognizer_partial_(recognizer);
  return value != nullptr ? std::string(value) : std::string();
}

std::string VoskApi::FinalResult(VoskRecognizer* recognizer) const {
  if (recognizer == nullptr || recognizer_final_ == nullptr) {
    return {};
  }
  const char* value = recognizer_final_(recognizer);
  return value != nullptr ? std::string(value) : std::string();
}

void VoskApi::Reset(VoskRecognizer* recognizer) const {
  if (recognizer != nullptr && recognizer_reset_ != nullptr) {
    recognizer_reset_(recognizer);
  }
}

void VoskApi::EnableWordTimings(VoskRecognizer* recognizer) const {
  if (recognizer != nullptr && recognizer_set_words_ != nullptr) {
    recognizer_set_words_(recognizer, 1);
  }
}

void VoskApi::EnablePartialWords(VoskRecognizer* recognizer, bool enabled) const {
  if (recognizer != nullptr && recognizer_set_partial_words_ != nullptr) {
    recognizer_set_partial_words_(recognizer, enabled ? 1 : 0);
  }
}

void VoskApi::ConfigureLogging(bool debug) const {
  if (set_log_level_ != nullptr) {
    set_log_level_(debug ? 0 : -1);
  }
}

// Utility helpers -----------------------------------------------------------

static std::string EscapeJson(const std::string& value) {
  std::ostringstream oss;
  for (unsigned char ch : value) {
    switch (ch) {
      case '\\':
        oss << "\\\\";
        break;
      case '"':
        oss << "\\\"";
        break;
      case '\b':
        oss << "\\b";
        break;
      case '\f':
        oss << "\\f";
        break;
      case '\n':
        oss << "\\n";
        break;
      case '\r':
        oss << "\\r";
        break;
      case '\t':
        oss << "\\t";
        break;
      default:
        if (ch < 0x20) {
          oss << "\\u" << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(ch) << std::nouppercase << std::dec;
        } else {
          oss << ch;
        }
        break;
    }
  }
  return oss.str();
}

static std::string BuildErrorJson(const std::string& message, bool permanent) {
  std::ostringstream oss;
  oss << "{\"errorMsg\":\"" << EscapeJson(message) << "\",\"permanent\":"
      << (permanent ? "true" : "false") << "}";
  return oss.str();
}

static std::string BuildRecognitionPayload(const std::string& text, double confidence,
                                           bool final_result) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3);
  double safe_confidence = confidence;
  if (safe_confidence < 0.0) {
    safe_confidence = -1.0;
  } else if (safe_confidence > 1.0) {
    safe_confidence = 1.0;
  }
  oss << "{\"alternates\":[{\"recognizedWords\":\"" << EscapeJson(text)
      << "\",\"confidence\":" << safe_confidence << "}],\"resultType\":"
      << (final_result ? kFinalResult : kPartialResult) << "}";
  return oss.str();
}

static std::string ExtractJsonText(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t key_pos = json.find(needle);
  if (key_pos == std::string::npos) {
    return {};
  }
  std::size_t colon = json.find(':', key_pos);
  if (colon == std::string::npos) {
    return {};
  }
  colon++;
  while (colon < json.size() && std::isspace(static_cast<unsigned char>(json[colon]))) {
    colon++;
  }
  if (colon >= json.size() || json[colon] != '"') {
    return {};
  }
  colon++;
  std::string value;
  while (colon < json.size()) {
    const char ch = json[colon];
    if (ch == '"') {
      break;
    }
    if (ch == '\\' && colon + 1 < json.size()) {
      const char next = json[colon + 1];
      switch (next) {
        case '\\':
          value.push_back('\\');
          break;
        case '"':
          value.push_back('"');
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 't':
          value.push_back('\t');
          break;
        default:
          value.push_back(next);
          break;
      }
      colon += 2;
      continue;
    }
    value.push_back(ch);
    colon++;
  }
  return value;
}

static double ExtractAverageConfidence(const std::string& json) {
  double sum = 0.0;
  int count = 0;
  std::size_t pos = 0;
  while (true) {
    pos = json.find("\"conf\"", pos);
    if (pos == std::string::npos) {
      break;
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
      break;
    }
    pos++;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
      pos++;
    }
    std::size_t end = pos;
    while (end < json.size()) {
      char ch = json[end];
      if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+') {
        end++;
      } else {
        break;
      }
    }
    if (end <= pos) {
      break;
    }
    try {
      const double value = std::stod(json.substr(pos, end - pos));
      sum += value;
      count++;
    } catch (...) {
      // ignore parsing failures
    }
    pos = end;
  }
  if (count == 0) {
    return -1.0;
  }
  return sum / static_cast<double>(count);
}

static double ComputeSoundLevel(const int16_t* buffer, int frames) {
  if (buffer == nullptr || frames <= 0) {
    return 0.0;
  }
  double accum = 0.0;
  for (int i = 0; i < frames; ++i) {
    const double normalized = static_cast<double>(buffer[i]) / 32768.0;
    accum += normalized * normalized;
  }
  const double rms = std::sqrt(accum / frames);
  double db = 20.0 * std::log10(rms + 1e-9) + 90.0;
  if (!std::isfinite(db) || db < 0.0) {
    db = 0.0;
  }
  if (db > 120.0) {
    db = 120.0;
  }
  return db;
}

static std::string DescribePaError(PaError error_code) {
  std::ostringstream oss;
  oss << "PortAudio error (" << error_code << "): " << Pa_GetErrorText(error_code);
  return oss.str();
}

static std::string ListAvailableInputDevices() {
  const PaError count = Pa_GetDeviceCount();
  if (count < 0) {
    return DescribePaError(count);
  }
  if (count == 0) {
    return "No input devices detected.";
  }
  std::ostringstream oss;
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (info == nullptr || info->maxInputChannels <= 0) {
      continue;
    }
    const PaHostApiInfo* api_info = Pa_GetHostApiInfo(info->hostApi);
    oss << "[" << i << "] " << (info->name != nullptr ? info->name : "unknown")
        << " (API: " << (api_info != nullptr && api_info->name != nullptr ? api_info->name : "unknown")
        << ", channels: " << info->maxInputChannels
        << ", default SR: " << info->defaultSampleRate << ")\n";
  }
  std::string result = oss.str();
  if (result.empty()) {
    return "No input-capable devices detected.";
  }
  return result;
}

static StreamOpenResult OpenInputStreamWithTimeout(
    const PaStreamParameters& params, int sample_rate,
    unsigned long frames_per_buffer, std::chrono::milliseconds timeout) {
  auto promise =
      std::make_shared<std::promise<StreamOpenResult>>();
  std::future<StreamOpenResult> future = promise->get_future();
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  std::thread([promise, cancelled, params, sample_rate,
               frames_per_buffer]() {
    PaStream* stream = nullptr;
    PaError err =
        Pa_OpenStream(&stream, &params, nullptr, sample_rate,
                      frames_per_buffer, paClipOff, nullptr, nullptr);
    if (cancelled->load()) {
      if (stream != nullptr) {
        Pa_CloseStream(stream);
        stream = nullptr;
      }
    }
    promise->set_value(StreamOpenResult{err, stream});
  }).detach();
  if (future.wait_for(timeout) == std::future_status::timeout) {
    cancelled->store(true);
    return StreamOpenResult{paTimedOut, nullptr, true};
  }
  auto result = future.get();
  result.timed_out = false;
  return result;
}

static std::string GuessLocaleFromModelPath(const std::string& path) {
  auto separator = path.find_last_of("/\\");
  std::string folder = separator == std::string::npos ? path : path.substr(separator + 1);
  for (auto& ch : folder) {
    if (ch == '_') {
      ch = '-';
    }
  }
  const std::vector<std::string> hints = {"en-us", "en-gb", "de-de", "fr-fr", "es-es", "pt-br"};
  std::string lowered = folder;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  for (const auto& hint : hints) {
    if (lowered.find(hint) != std::string::npos) {
      std::string result = hint;
      if (result.size() > 2) {
        result[2] = '-';
      }
      for (size_t i = 0; i < result.size(); ++i) {
        if (i == 0 || i == 3) {
          result[i] = static_cast<char>(std::toupper(result[i]));
        } else {
          result[i] = static_cast<char>(std::tolower(result[i]));
        }
      }
      return result;
    }
  }
  if (folder.size() >= 5 && (folder[2] == '-' || folder[2] == '_')) {
    std::string candidate = folder.substr(0, 5);
    candidate[2] = '-';
    for (size_t i = 0; i < candidate.size(); ++i) {
      if (i == 0 || i == 3) {
        candidate[i] = static_cast<char>(std::toupper(candidate[i]));
      } else {
        candidate[i] = static_cast<char>(std::tolower(candidate[i]));
      }
    }
    return candidate;
  }
  return "en-US";
}

}  // namespace

struct _SpeechToTextLinuxPlugin {
  GObject parent_instance;
  SpeechToTextLinuxPluginState* state;
  FlMethodChannel* channel;
  GMainContext* main_context;
};

G_DEFINE_TYPE(SpeechToTextLinuxPlugin, speech_to_text_linux_plugin, g_object_get_type())

static void DebugLog(SpeechToTextLinuxPlugin* self, const std::string& message) {
  if (self == nullptr || self->state == nullptr || !self->state->debug_logging) {
    return;
  }
  g_message("speech_to_text_linux: %s", message.c_str());
}

struct PendingStringInvoke {
  SpeechToTextLinuxPlugin* plugin;
  gchar* method;
  gchar* payload;
};

struct PendingDoubleInvoke {
  SpeechToTextLinuxPlugin* plugin;
  gchar* method;
  double value;
};

static void InvokeStringOnMain(SpeechToTextLinuxPlugin* self, const char* method,
                               const std::string& payload) {
  if (self == nullptr || self->channel == nullptr || self->main_context == nullptr) {
    return;
  }
  auto* data = new PendingStringInvoke{
      SPEECH_TO_TEXT_LINUX_PLUGIN(g_object_ref(self)),
      g_strdup(method),
      g_strdup(payload.c_str()),
  };
  g_main_context_invoke_full(
      self->main_context, G_PRIORITY_DEFAULT,
      [](gpointer user_data) -> gboolean {
        auto* data = static_cast<PendingStringInvoke*>(user_data);
        if (data->plugin->channel != nullptr) {
          g_autoptr(FlValue) value = fl_value_new_string(data->payload);
          fl_method_channel_invoke_method(data->plugin->channel, data->method, value,
                                          nullptr, nullptr, nullptr);
        }
        g_free(data->method);
        g_free(data->payload);
        g_object_unref(data->plugin);
        delete data;
        return G_SOURCE_REMOVE;
      },
      data, nullptr);
}

static void InvokeDoubleOnMain(SpeechToTextLinuxPlugin* self, const char* method,
                               double value) {
  if (self == nullptr || self->channel == nullptr || self->main_context == nullptr) {
    return;
  }
  auto* data = new PendingDoubleInvoke{
      SPEECH_TO_TEXT_LINUX_PLUGIN(g_object_ref(self)), g_strdup(method), value};
  g_main_context_invoke_full(
      self->main_context, G_PRIORITY_DEFAULT,
      [](gpointer user_data) -> gboolean {
        auto* data = static_cast<PendingDoubleInvoke*>(user_data);
        if (data->plugin->channel != nullptr) {
          g_autoptr(FlValue) value = fl_value_new_float(data->value);
          fl_method_channel_invoke_method(data->plugin->channel, data->method, value,
                                          nullptr, nullptr, nullptr);
        }
        g_free(data->method);
        g_object_unref(data->plugin);
        delete data;
        return G_SOURCE_REMOVE;
      },
      data, nullptr);
}

static void SendStatus(SpeechToTextLinuxPlugin* self, const std::string& status) {
  InvokeStringOnMain(self, "notifyStatus", status);
}

static void SendError(SpeechToTextLinuxPlugin* self, const std::string& message,
                      bool permanent) {
  InvokeStringOnMain(self, "notifyError", BuildErrorJson(message, permanent));
}

static void SendRecognition(SpeechToTextLinuxPlugin* self, const std::string& text,
                            double confidence, bool final_result) {
  InvokeStringOnMain(self, "textRecognition",
                     BuildRecognitionPayload(text, confidence, final_result));
}

static void SendSoundLevel(SpeechToTextLinuxPlugin* self, double level) {
  InvokeDoubleOnMain(self, "soundLevelChange", level);
}

static FlValue* LookupValue(FlValue* map, const char* key) {
  if (map == nullptr || fl_value_get_type(map) != FL_VALUE_TYPE_MAP) {
    return nullptr;
  }
  return fl_value_lookup_string(map, key);
}

static std::string GetStringArg(FlValue* map, const char* key) {
  FlValue* value = LookupValue(map, key);
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_STRING) {
    return {};
  }
  return fl_value_get_string(value);
}

static bool GetBoolArg(FlValue* map, const char* key, bool fallback) {
  FlValue* value = LookupValue(map, key);
  if (value == nullptr) {
    return fallback;
  }
  if (fl_value_get_type(value) == FL_VALUE_TYPE_BOOL) {
    return fl_value_get_bool(value);
  }
  if (fl_value_get_type(value) == FL_VALUE_TYPE_INT) {
    return fl_value_get_int(value) != 0;
  }
  return fallback;
}

static gint64 GetIntArg(FlValue* map, const char* key, gint64 fallback) {
  FlValue* value = LookupValue(map, key);
  if (value == nullptr) {
    return fallback;
  }
  switch (fl_value_get_type(value)) {
    case FL_VALUE_TYPE_INT:
      return fl_value_get_int(value);
    case FL_VALUE_TYPE_FLOAT:
      return static_cast<gint64>(fl_value_get_float(value));
    default:
      return fallback;
  }
}

static void CloseStreamLocked(SpeechToTextLinuxPluginState* state) {
  if (state->stream != nullptr) {
    Pa_CloseStream(state->stream);
    state->stream = nullptr;
  }
}

static void ReleaseRecognizerLocked(SpeechToTextLinuxPluginState* state) {
  if (state->recognizer != nullptr) {
    state->vosk.FreeRecognizer(state->recognizer);
    state->recognizer = nullptr;
  }
}

static void CaptureLoop(SpeechToTextLinuxPlugin* self) {
  SpeechToTextLinuxPluginState* state = self->state;
  if (state == nullptr) {
    return;
  }
  const int frames = static_cast<int>(state->frames_per_buffer);
  std::vector<int16_t> buffer(frames);
  state->ResetRecognitionTimers();

  while (!state->stop_requested.load()) {
    const PaError err = Pa_ReadStream(state->stream, buffer.data(), frames);
    if (err == paInputOverflowed) {
      continue;
    }
    if (err == paStreamIsStopped || err == paStreamIsNotStopped) {
      break;
    }
    if (err != paNoError) {
      SendError(self, DescribePaError(err), true);
      break;
    }
    SendSoundLevel(self, ComputeSoundLevel(buffer.data(), frames));
    const int accepted =
        state->vosk.AcceptWaveform(state->recognizer, buffer.data(), frames);
    if (accepted != 0) {
      const std::string json = state->vosk.Result(state->recognizer);
      const std::string text = ExtractJsonText(json, "text");
      if (!text.empty()) {
        state->reported_speech = true;
        state->last_speech_at = std::chrono::steady_clock::now();
        const double confidence = ExtractAverageConfidence(json);
        SendRecognition(self, text, confidence, true);
      }
    } else if (state->partial_results_enabled) {
      const std::string json = state->vosk.PartialResult(state->recognizer);
      const std::string text = ExtractJsonText(json, "partial");
      if (!text.empty() && text != state->last_partial_text) {
        state->reported_speech = true;
        state->last_partial_text = text;
        state->last_speech_at = std::chrono::steady_clock::now();
        SendRecognition(self, text, -1.0, false);
      }
    }

    const auto now = std::chrono::steady_clock::now();
    if (state->listen_timeout.count() > 0 &&
        now - state->listen_started >= state->listen_timeout) {
      break;
    }
    if (state->pause_timeout.count() > 0 && state->reported_speech &&
        now - state->last_speech_at >= state->pause_timeout) {
      break;
    }
  }

  if (!state->cancel_requested.load()) {
    const std::string final_json = state->vosk.FinalResult(state->recognizer);
    const std::string text = ExtractJsonText(final_json, "text");
    if (!text.empty()) {
      const double confidence = ExtractAverageConfidence(final_json);
      SendRecognition(self, text, confidence, true);
      state->reported_speech = true;
    }
  }

  SendStatus(self, "notListening");
  if (!state->cancel_requested.load()) {
    if (state->reported_speech) {
      SendStatus(self, "done");
    } else {
      SendStatus(self, "doneNoResult");
    }
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    CloseStreamLocked(state);
    ReleaseRecognizerLocked(state);
    state->listening = false;
  }
}

static void StopCaptureThread(SpeechToTextLinuxPluginState* state) {
  if (state == nullptr) {
    return;
  }
  if (state->stream != nullptr) {
    Pa_StopStream(state->stream);
    Pa_AbortStream(state->stream);
  }
  state->stop_requested.store(true);
  if (state->capture_thread_running) {
    if (state->capture_thread.joinable()) {
      state->capture_thread.join();
    }
    state->capture_thread_running = false;
  }
}

static FlMethodResponse* SuccessBool(bool value) {
  g_autoptr(FlValue) result = fl_value_new_bool(value);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse* SuccessNull() {
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

static FlMethodResponse* MakeError(const char* code, const std::string& message) {
  return FL_METHOD_RESPONSE(
      fl_method_error_response_new(code, message.c_str(), nullptr));
}

static FlMethodResponse* HandleHasPermission() {
  return SuccessBool(true);
}

static FlMethodResponse* HandleInitialize(SpeechToTextLinuxPlugin* self, FlValue* args) {
  SpeechToTextLinuxPluginState* state = self->state;
  if (state == nullptr) {
    return MakeError("state_unavailable", "Plugin state not initialized");
  }

  const bool debug = GetBoolArg(args, "debugLogging", false);
  const std::string model_path = GetStringArg(args, "modelPath");
  const std::string library_path = GetStringArg(args, "voskLibraryPath");

  if (model_path.empty()) {
    SendError(self, "Missing Vosk model path", true);
    return SuccessBool(false);
  }

  std::unique_lock<std::mutex> lock(state->mutex);
  state->debug_logging = debug;

  if (!state->vosk.Ready()) {
    if (!state->vosk.Load(library_path)) {
      SendError(self, state->vosk.last_error(), true);
      return SuccessBool(false);
    }
  }
  state->vosk.ConfigureLogging(debug);

  VoskModel* new_model = state->vosk.NewModel(model_path);
  if (new_model == nullptr) {
    SendError(self, "Failed to open Vosk model", true);
    return SuccessBool(false);
  }
  if (state->model != nullptr) {
    state->vosk.FreeModel(state->model);
  }
  state->model = new_model;
  state->model_path = model_path;

  if (!state->pa_initialized) {
    const PaError err = Pa_Initialize();
    if (err != paNoError) {
      state->vosk.FreeModel(state->model);
      state->model = nullptr;
      SendError(self, DescribePaError(err), true);
      return SuccessBool(false);
    }
    state->pa_initialized = true;
  }

  std::string locale = GetStringArg(args, "modelLocale");
  if (locale.empty()) {
    locale = GuessLocaleFromModelPath(model_path);
  }
  std::string display_name = GetStringArg(args, "modelDisplayName");
  if (display_name.empty()) {
    display_name = locale + " (Vosk)";
  }
  state->locale_tag = locale;
  state->locale_label = locale + ":" + display_name;
  state->initialized = true;

  DebugLog(self, "Vosk model loaded from " + model_path);
  return SuccessBool(true);
}

static FlMethodResponse* HandleListen(SpeechToTextLinuxPlugin* self, FlValue* args) {
  SpeechToTextLinuxPluginState* state = self->state;
  if (state == nullptr) {
    return MakeError("state_unavailable", "Plugin state not initialized");
  }
  std::unique_lock<std::mutex> lock(state->mutex);
  if (!state->initialized || state->model == nullptr) {
    SendError(self, "Speech engine not initialized", true);
    return SuccessBool(false);
  }
  if (state->listening) {
    DebugLog(self, "Already listening");
    return SuccessBool(false);
  }

  state->partial_results_enabled =
      GetBoolArg(args, "partialResults", true);
  state->sample_rate = static_cast<int>(GetIntArg(args, "sampleRate", state->sample_rate));
  if (state->sample_rate <= 0) {
    state->sample_rate = 16000;
  }
  state->listen_timeout =
      std::chrono::milliseconds(GetIntArg(args, "listenForMillis", 0));
  state->pause_timeout =
      std::chrono::milliseconds(GetIntArg(args, "pauseForMillis", 0));

  ReleaseRecognizerLocked(state);
  state->recognizer = state->vosk.NewRecognizer(state->model, state->sample_rate);
  if (state->recognizer == nullptr) {
    SendError(self, "Failed to create Vosk recognizer", true);
    return SuccessBool(false);
  }
  state->vosk.EnableWordTimings(state->recognizer);
  state->vosk.EnablePartialWords(state->recognizer, state->partial_results_enabled);

  PaStreamParameters input_params;
  input_params.device = Pa_GetDefaultInputDevice();
  if (input_params.device == paNoDevice) {
    std::ostringstream error;
    error << "No default input device. Detected devices: " << ListAvailableInputDevices();
    SendError(self, error.str(), true);
    ReleaseRecognizerLocked(state);
    return SuccessBool(false);
  }
  const PaDeviceInfo* device_info = Pa_GetDeviceInfo(input_params.device);
  input_params.channelCount = 1;
  input_params.sampleFormat = paInt16;
  input_params.suggestedLatency = device_info != nullptr ? device_info->defaultLowInputLatency : 0.0;
  input_params.hostApiSpecificStreamInfo = nullptr;

  state->frames_per_buffer = 1024;
  const PaStreamParameters params_copy = input_params;
  lock.unlock();
  auto open_result = OpenInputStreamWithTimeout(
      params_copy, state->sample_rate, state->frames_per_buffer,
      std::chrono::seconds(2));
  lock.lock();
  if (open_result.timed_out) {
    std::ostringstream error;
    error << "Timed out while opening audio input. Detected devices: "
          << ListAvailableInputDevices();
    SendError(self, error.str(), true);
    ReleaseRecognizerLocked(state);
    return SuccessBool(false);
  }
  if (open_result.error != paNoError) {
    SendError(self, DescribePaError(open_result.error), true);
    if (open_result.stream != nullptr) {
      Pa_CloseStream(open_result.stream);
    }
    ReleaseRecognizerLocked(state);
    return SuccessBool(false);
  }
  state->stream = open_result.stream;

  const PaError start_error = Pa_StartStream(state->stream);
  if (start_error != paNoError) {
    SendError(self, DescribePaError(start_error), true);
    CloseStreamLocked(state);
    ReleaseRecognizerLocked(state);
    return SuccessBool(false);
  }

  state->stop_requested.store(false);
  state->cancel_requested.store(false);
  state->listening = true;
  state->capture_thread_running = true;
  state->capture_thread = std::thread(CaptureLoop, self);

  SendStatus(self, "listening");
  DebugLog(self, "Listening started");
  return SuccessBool(true);
}

static FlMethodResponse* HandleStop(SpeechToTextLinuxPlugin* self, bool cancel) {
  SpeechToTextLinuxPluginState* state = self->state;
  if (state == nullptr) {
    return SuccessNull();
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->listening) {
      return SuccessNull();
    }
    state->cancel_requested.store(cancel);
    state->stop_requested.store(true);
  }
  StopCaptureThread(state);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    CloseStreamLocked(state);
    ReleaseRecognizerLocked(state);
    state->listening = false;
  }
  return SuccessNull();
}

static FlMethodResponse* HandleLocales(SpeechToTextLinuxPlugin* self) {
  g_autoptr(FlValue) locales = fl_value_new_list();
  if (self->state != nullptr) {
    std::lock_guard<std::mutex> lock(self->state->mutex);
    if (!self->state->locale_label.empty()) {
      fl_value_append_take(locales,
                           fl_value_new_string(self->state->locale_label.c_str()));
    }
  }
  return FL_METHOD_RESPONSE(fl_method_success_response_new(locales));
}

static void speech_to_text_linux_plugin_handle_method_call(
    SpeechToTextLinuxPlugin* self, FlMethodCall* method_call) {
  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);
  g_autoptr(FlMethodResponse) response = nullptr;

  if (strcmp(method, "hasPermission") == 0) {
    response = HandleHasPermission();
  } else if (strcmp(method, "initialize") == 0) {
    response = HandleInitialize(self, args);
  } else if (strcmp(method, "listen") == 0) {
    response = HandleListen(self, args);
  } else if (strcmp(method, "stop") == 0) {
    response = HandleStop(self, false);
  } else if (strcmp(method, "cancel") == 0) {
    response = HandleStop(self, true);
  } else if (strcmp(method, "locales") == 0) {
    response = HandleLocales(self);
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

static void speech_to_text_linux_plugin_dispose(GObject* object) {
  SpeechToTextLinuxPlugin* self = SPEECH_TO_TEXT_LINUX_PLUGIN(object);
  if (self->state != nullptr) {
    StopCaptureThread(self->state);
    delete self->state;
    self->state = nullptr;
  }
  if (self->channel != nullptr) {
    g_clear_object(&self->channel);
  }
  if (self->main_context != nullptr) {
    g_main_context_unref(self->main_context);
    self->main_context = nullptr;
  }
  G_OBJECT_CLASS(speech_to_text_linux_plugin_parent_class)->dispose(object);
}

static void speech_to_text_linux_plugin_class_init(SpeechToTextLinuxPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = speech_to_text_linux_plugin_dispose;
}

static void speech_to_text_linux_plugin_init(SpeechToTextLinuxPlugin* self) {
  self->state = new SpeechToTextLinuxPluginState();
  self->channel = nullptr;
  self->main_context = g_main_context_ref_thread_default();
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  SpeechToTextLinuxPlugin* plugin = SPEECH_TO_TEXT_LINUX_PLUGIN(user_data);
  speech_to_text_linux_plugin_handle_method_call(plugin, method_call);
}

void speech_to_text_linux_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  SpeechToTextLinuxPlugin* plugin = SPEECH_TO_TEXT_LINUX_PLUGIN(
      g_object_new(speech_to_text_linux_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "speech_to_text_linux", FL_METHOD_CODEC(codec));
  plugin->channel = FL_METHOD_CHANNEL(g_object_ref(channel));

  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin), g_object_unref);

  g_object_unref(plugin);
}
