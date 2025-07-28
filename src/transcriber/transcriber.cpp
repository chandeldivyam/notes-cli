#include "transcriber.h"
#include "whisper.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <unistd.h>
#include <sstream>

StreamingTranscriber::StreamingTranscriber(const TranscriptionConfig& config)
    : config_(config)
    , whisper_ctx_(nullptr)
    , whisper_state_(nullptr)
    , running_energy_avg_(0.0f)
    , last_chunk_timestamp_(0.0f)
    , smart_chunker_(std::make_unique<SmartChunker>(config)) {
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
    
    std::vector<float> read_buffer;
    read_buffer.reserve(4096); // Buffer for reading from pipe
    
    auto start_time = std::chrono::steady_clock::now();
    
    while (is_running_.load()) {
        // Read audio data in chunks
        float sample;
        if (pipe.read(reinterpret_cast<char*>(&sample), sizeof(float))) {
            read_buffer.push_back(sample);
            
            // Process buffer periodically (every 1024 samples ~64ms at 16kHz)
            if (read_buffer.size() >= 1024) {
                auto current_time = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time);
                float timestamp = elapsed.count() / 1000.0f;
                
                // Use smart chunking if enabled, otherwise use fixed chunking
                if (config_.enable_smart_chunking) {
                    auto chunk = smart_chunker_->processAudio(read_buffer, timestamp);
                    if (chunk.has_value()) {
                        // Add to processing queue
                        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                        if (audio_queue_.size() < MAX_QUEUE_SIZE) {
                            audio_queue_.emplace(chunk->audio, chunk->timestamp);
                            audio_queue_cv_.notify_one();
                        }
                    }
                } else {
                    // Original fixed chunking logic
                    const int chunk_samples = (config_.chunk_duration_ms * SAMPLE_RATE) / 1000;
                    if (read_buffer.size() >= chunk_samples) {
                        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                        if (audio_queue_.size() < MAX_QUEUE_SIZE) {
                            audio_queue_.emplace(read_buffer, timestamp);
                            audio_queue_cv_.notify_one();
                        }
                        
                        // Keep overlap for next chunk
                        const int overlap_samples = (config_.overlap_ms * SAMPLE_RATE) / 1000;
                        if (overlap_samples > 0 && read_buffer.size() > overlap_samples) {
                            std::vector<float> overlap_data(
                                read_buffer.end() - overlap_samples, read_buffer.end()
                            );
                            read_buffer = std::move(overlap_data);
                        } else {
                            read_buffer.clear();
                        }
                    }
                }
                
                // Clear read buffer for smart chunking (it manages its own buffer)
                if (config_.enable_smart_chunking) {
                    read_buffer.clear();
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
    
    // Transcribe with or without context
    TranscriptionResult result;
    if (config_.enable_context) {
        result = transcribeWithContext(audio_data, timestamp);
    } else {
        result = transcribeChunk(audio_data, timestamp);
    }
    
    // Update context for next transcription
    if (config_.enable_context && !result.text.empty()) {
        updateContext(result, audio_data);
    }
    
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
    wparams.no_context = false;
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

// SmartChunker Implementation
SmartChunker::SmartChunker(const TranscriptionConfig& config)
    : config_(config)
    , last_speech_time_(0.0f)
    , vad_(config.silence_threshold) {
}

std::optional<AudioChunk> SmartChunker::processAudio(const std::vector<float>& new_audio, float timestamp) {
    buffer_.insert(buffer_.end(), new_audio.begin(), new_audio.end());
    
    int samples_per_ms = SAMPLE_RATE / 1000;
    int min_samples = config_.min_chunk_duration_ms * samples_per_ms;
    int max_samples = config_.max_chunk_duration_ms * samples_per_ms;
    int optimal_samples = config_.optimal_chunk_duration_ms * samples_per_ms;
    
    // Don't process if we don't have minimum chunk
    if (buffer_.size() < min_samples) {
        return std::nullopt;
    }
    
    // Look for natural break points (silence) around optimal size
    if (buffer_.size() >= optimal_samples) {
        int silence_samples = config_.min_silence_duration_ms * samples_per_ms;
        
        // Search for silence window starting from optimal size
        for (int i = optimal_samples; i < buffer_.size() - silence_samples && i < max_samples; i++) {
            if (isSilentWindow(buffer_, i, silence_samples)) {
                // Found good break point
                return extractChunk(i + silence_samples / 2);
            }
        }
    }
    
    // Force chunk at max duration
    if (buffer_.size() >= max_samples) {
        return extractChunk(max_samples);
    }
    
    return std::nullopt;
}

void SmartChunker::reset() {
    buffer_.clear();
    last_speech_time_ = 0.0f;
    vad_.reset();
}

bool SmartChunker::isSilentWindow(const std::vector<float>& audio, int start, int length) {
    for (int i = start; i < start + length && i < audio.size(); i++) {
        if (std::abs(audio[i]) > config_.silence_threshold) {
            return false;
        }
    }
    return true;
}

AudioChunk SmartChunker::extractChunk(int samples) {
    AudioChunk chunk;
    chunk.audio.assign(buffer_.begin(), buffer_.begin() + samples);
    chunk.timestamp = last_speech_time_;
    chunk.is_final = false;
    
    // Keep overlap for context (2 seconds)
    int overlap_samples = 2 * SAMPLE_RATE;
    if (samples > overlap_samples) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + (samples - overlap_samples));
    } else {
        buffer_.clear();
    }
    
    last_speech_time_ += float(samples - overlap_samples) / SAMPLE_RATE;
    return chunk;
}

// Context Management Implementation
TranscriptionResult StreamingTranscriber::transcribeWithContext(const std::vector<float>& audio_data, float timestamp) {
    // Prepare audio with context
    std::vector<float> contextual_audio = prepareContextualAudio(audio_data);
    
    // Prepare context prompt
    std::string context_prompt;
    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        context_prompt = prepareContextPrompt(context_.previous_text);
    }
    
    TranscriptionResult result;
    result.timestamp = timestamp;
    result.is_partial = false;
    result.confidence = 0.0f;
    
    // Prepare whisper parameters with context
    struct whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    
    wparams.strategy = WHISPER_SAMPLING_GREEDY;
    wparams.n_threads = config_.threads;
    wparams.n_max_text_ctx = 16384;
    wparams.language = config_.language.c_str();
    wparams.translate = config_.translate;
    wparams.no_context = false;
    wparams.single_segment = false;
    wparams.print_realtime = false;
    wparams.print_progress = false;
    wparams.print_timestamps = false;
    wparams.print_special = false;
    wparams.suppress_blank = true;
    wparams.suppress_non_speech_tokens = true;
    wparams.temperature = config_.temperature;
    wparams.max_tokens = config_.max_tokens;
    
    // Set context prompt
    wparams.initial_prompt = context_prompt.empty() ? nullptr : context_prompt.c_str();
    
    // Run transcription
    if (whisper_full_with_state(whisper_ctx_, whisper_state_, wparams, 
                               contextual_audio.data(), contextual_audio.size()) != 0) {
        std::cerr << "❌ Contextual transcription failed" << std::endl;
        return result;
    }
    
    // Extract results
    const int n_segments = whisper_full_n_segments_from_state(whisper_state_);
    std::string transcription;
    
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text_from_state(whisper_state_, i);
        if (text && strlen(text) > 0) {
            std::string segment_text(text);
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
    
    // Calculate confidence
    if (n_segments > 0) {
        result.confidence = 0.8f; // Placeholder
    }
    
    // Remove contextual overlap if enabled
    if (config_.remove_context_overlap && !context_.previous_text.empty()) {
        result = removeContextualOverlap(result);
    }
    
    return result;
}

void StreamingTranscriber::updateContext(const TranscriptionResult& result, const std::vector<float>& audio_data) {
    std::lock_guard<std::mutex> lock(context_mutex_);
    
    // Update text context
    context_.previous_text = result.text;
    context_.timestamp = result.timestamp;
    
    // Count words for prompt truncation
    context_.word_count = 0;
    std::istringstream iss(result.text);
    std::string word;
    while (iss >> word) {
        context_.word_count++;
    }
    
    // Update audio context (keep last N seconds)
    int context_samples = (config_.context_duration_ms * SAMPLE_RATE) / 1000;
    if (audio_data.size() <= context_samples) {
        context_.previous_audio = audio_data;
    } else {
        context_.previous_audio.assign(
            audio_data.end() - context_samples, 
            audio_data.end()
        );
    }
}

std::string StreamingTranscriber::prepareContextPrompt(const std::string& previous_text) {
    if (previous_text.empty()) {
        return "";
    }
    
    // Truncate prompt if too long
    std::istringstream iss(previous_text);
    std::vector<std::string> words;
    std::string word;
    
    while (iss >> word) {
        words.push_back(word);
    }
    
    // Keep last N words to stay under token limit
    int max_words = std::min(config_.max_prompt_tokens / 2, (int)words.size());
    
    if (max_words <= 0) {
        return "";
    }
    
    std::string prompt;
    for (int i = words.size() - max_words; i < words.size(); i++) {
        if (!prompt.empty()) {
            prompt += " ";
        }
        prompt += words[i];
    }
    
    return prompt;
}

TranscriptionResult StreamingTranscriber::removeContextualOverlap(const TranscriptionResult& result) {
    // Simple overlap removal - look for common words at the beginning
    TranscriptionResult clean_result = result;
    
    if (context_.previous_text.empty() || result.text.empty()) {
        return clean_result;
    }
    
    // Split into words
    std::istringstream prev_iss(context_.previous_text);
    std::istringstream curr_iss(result.text);
    
    std::vector<std::string> prev_words;
    std::vector<std::string> curr_words;
    
    std::string word;
    while (prev_iss >> word) prev_words.push_back(word);
    while (curr_iss >> word) curr_words.push_back(word);
    
    // Find overlap (last words of previous match first words of current)
    int overlap_count = 0;
    int max_check = std::min(prev_words.size(), curr_words.size());
    max_check = std::min(max_check, 10); // Don't check more than 10 words
    
    for (int i = 1; i <= max_check; i++) {
        bool matches = true;
        for (int j = 0; j < i; j++) {
            if (prev_words[prev_words.size() - i + j] != curr_words[j]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            overlap_count = i;
        }
    }
    
    // Remove overlapping words from current result
    if (overlap_count > 0 && overlap_count < curr_words.size()) {
        std::string clean_text;
        for (int i = overlap_count; i < curr_words.size(); i++) {
            if (!clean_text.empty()) {
                clean_text += " ";
            }
            clean_text += curr_words[i];
        }
        clean_result.text = clean_text;
    }
    
    return clean_result;
}

std::vector<float> StreamingTranscriber::prepareContextualAudio(const std::vector<float>& current_audio) {
    std::lock_guard<std::mutex> lock(context_mutex_);
    
    // If no previous context, return current audio
    if (context_.previous_audio.empty()) {
        return current_audio;
    }
    
    // Combine previous context audio with current audio
    std::vector<float> contextual_audio;
    contextual_audio.reserve(context_.previous_audio.size() + current_audio.size());
    
    // Add previous audio context
    contextual_audio.insert(contextual_audio.end(), 
                           context_.previous_audio.begin(), 
                           context_.previous_audio.end());
    
    // Add current audio
    contextual_audio.insert(contextual_audio.end(), 
                           current_audio.begin(), 
                           current_audio.end());
    
    return contextual_audio;
}