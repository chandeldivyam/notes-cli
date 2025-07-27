#include "transcriber.h"
#include "whisper.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <unistd.h>

StreamingTranscriber::StreamingTranscriber(const TranscriptionConfig& config)
    : config_(config)
    , whisper_ctx_(nullptr)
    , whisper_state_(nullptr)
    , running_energy_avg_(0.0f)
    , last_chunk_timestamp_(0.0f) {
}

StreamingTranscriber::~StreamingTranscriber() {
    stop();
    
    if (whisper_state_) {
        whisper_free_state(whisper_state_);
    }
    if (whisper_ctx_) {
        whisper_free(whisper_ctx_);
    }
}

bool StreamingTranscriber::initialize() {
    std::cout << "🤖 Loading Whisper model: " << config_.model_path << std::endl;
    
    // Initialize whisper context
    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true; // Enable Metal acceleration on macOS
    
    whisper_ctx_ = whisper_init_from_file_with_params(config_.model_path.c_str(), cparams);
    if (!whisper_ctx_) {
        std::cerr << "❌ Failed to load model: " << config_.model_path << std::endl;
        return false;
    }
    
    // Create whisper state for thread-safe processing
    whisper_state_ = whisper_init_state(whisper_ctx_);
    if (!whisper_state_) {
        std::cerr << "❌ Failed to create whisper state" << std::endl;
        return false;
    }
    
    std::cout << "✅ Model loaded successfully" << std::endl;
    std::cout << "🧠 Threads: " << config_.threads << std::endl;
    std::cout << "🌍 Language: " << config_.language << std::endl;
    
    return true;
}

void StreamingTranscriber::start(const std::string& pipe_path, TranscriptionCallback callback) {
    if (is_running_.load()) {
        std::cerr << "⚠️ Transcriber is already running" << std::endl;
        return;
    }
    
    callback_ = callback;
    is_running_.store(true);
    
    // Start threads
    audio_reader_thread_ = std::thread(&StreamingTranscriber::audioReaderThread, this, pipe_path);
    transcription_thread_ = std::thread(&StreamingTranscriber::transcriptionThread, this);
    
    std::cout << "🎯 Streaming transcription started" << std::endl;
}

void StreamingTranscriber::stop() {
    if (!is_running_.load()) {
        return;
    }
    
    is_running_.store(false);
    audio_queue_cv_.notify_all();
    
    if (audio_reader_thread_.joinable()) {
        audio_reader_thread_.join();
    }
    if (transcription_thread_.joinable()) {
        transcription_thread_.join();
    }
    
    std::cout << "🛑 Transcription stopped" << std::endl;
}

void StreamingTranscriber::audioReaderThread(const std::string& pipe_path) {
    std::ifstream pipe(pipe_path, std::ios::binary);
    if (!pipe.is_open()) {
        std::cerr << "❌ Failed to open pipe: " << pipe_path << std::endl;
        return;
    }
    
    const int chunk_samples = (config_.chunk_duration_ms * SAMPLE_RATE) / 1000;
    const int overlap_samples = (config_.overlap_ms * SAMPLE_RATE) / 1000;
    
    std::vector<float> buffer;
    buffer.reserve(chunk_samples);
    
    auto start_time = std::chrono::steady_clock::now();
    
    while (is_running_.load()) {
        // Read audio data
        float sample;
        if (pipe.read(reinterpret_cast<char*>(&sample), sizeof(float))) {
            buffer.push_back(sample);
            
            // Process when we have enough samples
            if (buffer.size() >= chunk_samples) {
                auto current_time = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time);
                float timestamp = elapsed.count() / 1000.0f;
                
                // Add to processing queue
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    if (audio_queue_.size() < MAX_QUEUE_SIZE) {
                        audio_queue_.emplace(buffer, timestamp);
                        audio_queue_cv_.notify_one();
                    }
                }
                
                // Keep overlap for next chunk
                if (overlap_samples > 0 && buffer.size() > overlap_samples) {
                    std::vector<float> overlap_data(
                        buffer.end() - overlap_samples, buffer.end()
                    );
                    buffer = std::move(overlap_data);
                } else {
                    buffer.clear();
                }
            }
        } else {
            // Pipe closed or error
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void StreamingTranscriber::transcriptionThread() {
    while (is_running_.load()) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this] {
            return !audio_queue_.empty() || !is_running_.load();
        });
        
        if (!is_running_.load()) {
            break;
        }
        
        if (!audio_queue_.empty()) {
            auto [audio_data, timestamp] = std::move(audio_queue_.front());
            audio_queue_.pop();
            lock.unlock();
            
            processAudioChunk(audio_data, timestamp);
        }
    }
}

void StreamingTranscriber::processAudioChunk(const std::vector<float>& audio_data, float timestamp) {
    // Apply VAD if enabled
    if (config_.enable_vad && !detectVoiceActivity(audio_data)) {
        return;
    }
    
    // Transcribe the chunk
    auto result = transcribeChunk(audio_data, timestamp);
    
    // Call callback with result
    if (!result.text.empty() && callback_) {
        callback_(result);
    }
}

bool StreamingTranscriber::detectVoiceActivity(const std::vector<float>& audio_data) {
    if (!config_.enable_vad) {
        return true;
    }
    
    // Calculate RMS energy
    float energy = 0.0f;
    for (float sample : audio_data) {
        energy += sample * sample;
    }
    energy = std::sqrt(energy / audio_data.size());
    
    // Update running average
    const float alpha = 0.1f; // Smoothing factor
    running_energy_avg_ = alpha * energy + (1.0f - alpha) * running_energy_avg_;
    
    // Voice activity threshold
    return energy > config_.vad_threshold * running_energy_avg_;
}

TranscriptionResult StreamingTranscriber::transcribeChunk(const std::vector<float>& audio_data, float timestamp) {
    TranscriptionResult result;
    result.timestamp = timestamp;
    result.is_partial = false;
    result.confidence = 0.0f;
    
    // Prepare whisper parameters
    struct whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    
    wparams.strategy = WHISPER_SAMPLING_GREEDY;
    wparams.n_threads = config_.threads;
    wparams.n_max_text_ctx = 16384;
    wparams.language = config_.language.c_str();
    wparams.translate = config_.translate;
    wparams.no_context = true;
    wparams.single_segment = false;
    wparams.print_realtime = false;
    wparams.print_progress = false;
    wparams.print_timestamps = false;
    wparams.print_special = false;
    wparams.suppress_blank = true;
    wparams.suppress_non_speech_tokens = true;
    wparams.temperature = config_.temperature;
    wparams.max_tokens = config_.max_tokens;
    
    // Run transcription
    if (whisper_full_with_state(whisper_ctx_, whisper_state_, wparams, 
                               audio_data.data(), audio_data.size()) != 0) {
        std::cerr << "❌ Transcription failed" << std::endl;
        return result;
    }
    
    // Extract results
    const int n_segments = whisper_full_n_segments_from_state(whisper_state_);
    std::string transcription;
    
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text_from_state(whisper_state_, i);
        if (text && strlen(text) > 0) {
            // Clean up the text
            std::string segment_text(text);
            
            // Remove leading/trailing whitespace
            segment_text.erase(0, segment_text.find_first_not_of(" \t\n\r"));
            segment_text.erase(segment_text.find_last_not_of(" \t\n\r") + 1);
            
            if (!segment_text.empty()) {
                if (!transcription.empty()) {
                    transcription += " ";
                }
                transcription += segment_text;
            }
        }
    }
    
    result.text = transcription;
    
    // Calculate average confidence (simplified)
    if (n_segments > 0) {
        float total_confidence = 0.0f;
        for (int i = 0; i < n_segments; ++i) {
            // Whisper doesn't provide direct confidence, so we use a heuristic
            // based on segment length and audio energy
            total_confidence += 0.8f; // Placeholder confidence
        }
        result.confidence = total_confidence / n_segments;
    }
    
    return result;
}

// Voice Activity Detector Implementation
VoiceActivityDetector::VoiceActivityDetector(float threshold, int window_size)
    : threshold_(threshold)
    , window_size_(window_size)
    , background_energy_(0.0f)
    , frame_count_(0) {
    energy_buffer_.reserve(window_size);
}

bool VoiceActivityDetector::isVoiceActive(const std::vector<float>& audio_data) {
    float energy = calculateEnergy(audio_data);
    
    // Update background energy estimate
    updateBackgroundEnergy(energy);
    
    // Add to energy buffer
    energy_buffer_.push_back(energy);
    if (energy_buffer_.size() > window_size_) {
        energy_buffer_.erase(energy_buffer_.begin());
    }
    
    // Calculate average energy over window
    float avg_energy = 0.0f;
    for (float e : energy_buffer_) {
        avg_energy += e;
    }
    avg_energy /= energy_buffer_.size();
    
    // Voice activity decision
    return avg_energy > threshold_ * background_energy_;
}

void VoiceActivityDetector::reset() {
    energy_buffer_.clear();
    background_energy_ = 0.0f;
    frame_count_ = 0;
}

float VoiceActivityDetector::calculateEnergy(const std::vector<float>& audio_data) {
    float energy = 0.0f;
    for (float sample : audio_data) {
        energy += sample * sample;
    }
    return std::sqrt(energy / audio_data.size());
}

void VoiceActivityDetector::updateBackgroundEnergy(float energy) {
    const float alpha = 0.01f; // Very slow adaptation
    
    if (frame_count_ == 0) {
        background_energy_ = energy;
    } else {
        // Only update background if current energy is low (likely background)
        if (energy < background_energy_ * 2.0f) {
            background_energy_ = alpha * energy + (1.0f - alpha) * background_energy_;
        }
    }
    
    frame_count_++;
}