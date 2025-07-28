#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

struct whisper_context;
struct whisper_state;

// Forward declarations
class SmartChunker;

struct TranscriptionConfig {
    std::string model_path;
    std::string language = "en";
    bool translate = false;
    int threads = 4;
    float temperature = 0.0f;
    int max_tokens = 224;
    bool enable_vad = true;
    float vad_threshold = 0.6f;
    int chunk_duration_ms = 3000;
    int overlap_ms = 500;
    bool timestamps = true;
    
    // Smart chunking parameters
    int min_chunk_duration_ms = 5000;    // 5 seconds minimum
    int max_chunk_duration_ms = 30000;   // 30 seconds maximum  
    int optimal_chunk_duration_ms = 10000; // 10 seconds optimal
    float silence_threshold = 0.02f;
    int min_silence_duration_ms = 300;   // 300ms silence to split
    bool enable_smart_chunking = true;
    
    // Context management parameters
    bool enable_context = true;
    int context_duration_ms = 2000;      // 2 seconds of audio context
    int max_prompt_tokens = 200;         // Max tokens for context prompt
    bool remove_context_overlap = true;  // Remove overlap from final output
};

struct TranscriptionResult {
    std::string text;
    float timestamp;
    float confidence;
    bool is_partial;
};

struct ContextWindow {
    std::string previous_text;
    std::vector<float> previous_audio;
    float timestamp;
    int word_count = 0;
};

using TranscriptionCallback = std::function<void(const TranscriptionResult&)>;

class StreamingTranscriber {
public:
    StreamingTranscriber(const TranscriptionConfig& config);
    ~StreamingTranscriber();
    
    bool initialize();
    void start(const std::string& pipe_path, TranscriptionCallback callback);
    void stop();
    bool isRunning() const { return is_running_.load(); }
    
private:
    void audioReaderThread(const std::string& pipe_path);
    void transcriptionThread();
    void processAudioChunk(const std::vector<float>& audio_data, float timestamp);
    bool detectVoiceActivity(const std::vector<float>& audio_data);
    TranscriptionResult transcribeChunk(const std::vector<float>& audio_data, float timestamp);
    
    // Context management methods
    TranscriptionResult transcribeWithContext(const std::vector<float>& audio_data, float timestamp);
    void updateContext(const TranscriptionResult& result, const std::vector<float>& audio_data);
    std::string prepareContextPrompt(const std::string& previous_text);
    TranscriptionResult removeContextualOverlap(const TranscriptionResult& result);
    std::vector<float> prepareContextualAudio(const std::vector<float>& current_audio);
    
    TranscriptionConfig config_;
    whisper_context* whisper_ctx_;
    whisper_state* whisper_state_;
    
    std::atomic<bool> is_running_{false};
    std::thread audio_reader_thread_;
    std::thread transcription_thread_;
    
    // Audio processing
    std::queue<std::pair<std::vector<float>, float>> audio_queue_;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    
    // VAD state
    std::vector<float> vad_buffer_;
    float running_energy_avg_;
    
    // Callback
    TranscriptionCallback callback_;
    
    // Overlap handling
    std::vector<float> overlap_buffer_;
    float last_chunk_timestamp_;
    
    // Smart chunking
    std::unique_ptr<SmartChunker> smart_chunker_;
    
    // Context management
    ContextWindow context_;
    std::mutex context_mutex_;
    
    static constexpr int SAMPLE_RATE = 16000;
    static constexpr int MAX_QUEUE_SIZE = 10;
};

class VoiceActivityDetector {
public:
    VoiceActivityDetector(float threshold = 0.6f, int window_size = 512);
    
    bool isVoiceActive(const std::vector<float>& audio_data);
    void reset();
    
private:
    float threshold_;
    int window_size_;
    std::vector<float> energy_buffer_;
    float background_energy_;
    int frame_count_;
    
    float calculateEnergy(const std::vector<float>& audio_data);
    void updateBackgroundEnergy(float energy);
};

struct AudioChunk {
    std::vector<float> audio;
    float timestamp;
    bool is_final = false;
};

class SmartChunker {
public:
    SmartChunker(const TranscriptionConfig& config);
    
    std::optional<AudioChunk> processAudio(const std::vector<float>& new_audio, float timestamp);
    void reset();
    
private:
    TranscriptionConfig config_;
    std::vector<float> buffer_;
    float last_speech_time_;
    VoiceActivityDetector vad_;
    
    bool isSilentWindow(const std::vector<float>& audio, int start, int length);
    AudioChunk extractChunk(int samples);
    
    static constexpr int SAMPLE_RATE = 16000;
};