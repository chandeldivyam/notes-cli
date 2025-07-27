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

struct whisper_context;
struct whisper_state;

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
};

struct TranscriptionResult {
    std::string text;
    float timestamp;
    float confidence;
    bool is_partial;
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