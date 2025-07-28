#include <iostream>
#include <fstream>
#include <thread>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <set>
#include <cctype>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <getopt.h>
#include "transcriber/transcriber.h"

namespace fs = std::filesystem;

// Global flag for clean shutdown
std::atomic<bool> g_shutdown{false};

struct AppConfig {
    std::string output_file = "transcript.txt";
    bool timestamps = true;
    bool real_time_display = true;
    bool save_audio = false;
    std::string model_path = "models/ggml-base.en.bin";
    std::string language = "en";
    bool translate = false;
    int threads = 4;
    int sample_rate = 16000;
    int channels = 1;
    bool enable_vad = true;
    float vad_threshold = 0.6f;
    int chunk_duration_ms = 3000;
    int overlap_ms = 500;
    int max_latency_ms = 1000;
    bool verbose = false;
};

class RealTimeTranscriptionApp {
private:
    AppConfig config_;
    std::unique_ptr<StreamingTranscriber> transcriber_;
    std::ofstream output_stream_;
    std::string pipe_path_;
    pid_t capture_pid_ = -1;
    std::atomic<int> total_chunks_{0};
    std::atomic<int> transcribed_chunks_{0};
    std::chrono::steady_clock::time_point start_time_;
    
public:
    RealTimeTranscriptionApp(const AppConfig& config) : config_(config) {
        pipe_path_ = "/tmp/audio_transcriber_" + std::to_string(getpid());
    }
    
    ~RealTimeTranscriptionApp() {
        cleanup();
    }
    
    bool initialize() {
        if (!fs::exists(config_.model_path)) {
            std::cerr << "❌ Model not found: " << config_.model_path << std::endl;
            return false;
        }
        
        TranscriptionConfig transcription_config;
        transcription_config.model_path = config_.model_path;
        transcription_config.language = config_.language;
        transcription_config.translate = config_.translate;
        transcription_config.threads = config_.threads;
        transcription_config.enable_vad = config_.enable_vad;
        transcription_config.vad_threshold = config_.vad_threshold;
        transcription_config.chunk_duration_ms = config_.chunk_duration_ms;
        transcription_config.overlap_ms = config_.overlap_ms;
        transcription_config.timestamps = config_.timestamps;
        
        transcriber_ = std::make_unique<StreamingTranscriber>(transcription_config);
        
        if (!transcriber_->initialize()) {
            return false;
        }
        
        output_stream_.open(config_.output_file, std::ios::app);
        if (!output_stream_.is_open()) {
            std::cerr << "❌ Failed to open output file: " << config_.output_file << std::endl;
            return false;
        }
        
        return true;
    }
    
    void run() {
        setupSignalHandlers();
        writeSessionHeader();
        
        if (!createNamedPipe()) {
            return;
        }
        
        if (!startAudioCapture()) {
            return;
        }
        
        startTranscription();
        
        // Main loop with proper shutdown handling
        while (!g_shutdown.load() && transcriber_->isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        std::cout << "\n🛑 Shutting down..." << std::endl;
    }
    
private:
    void setupSignalHandlers() {
        // Proper signal handling that actually stops the application
        std::signal(SIGINT, [](int) {
            std::cout << "\n🛑 Received interrupt signal..." << std::endl;
            g_shutdown.store(true);
        });
        
        std::signal(SIGTERM, [](int) {
            std::cout << "\n🛑 Received termination signal..." << std::endl;
            g_shutdown.store(true);
        });
    }
    
    void writeSessionHeader() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        
        output_stream_ << "\n" << std::string(50, '=') << std::endl;
        output_stream_ << "🎙️  TRANSCRIPTION SESSION" << std::endl;
        output_stream_ << std::string(50, '=') << std::endl;
        output_stream_ << "Started: " << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << std::endl;
        output_stream_ << "Model: " << config_.model_path << std::endl;
        output_stream_ << "Language: " << config_.language << std::endl;
        output_stream_ << "Sample Rate: " << config_.sample_rate << "Hz" << std::endl;
        output_stream_ << "VAD: " << (config_.enable_vad ? "Enabled" : "Disabled") << std::endl;
        if (config_.enable_vad) {
            output_stream_ << "VAD Threshold: " << config_.vad_threshold << std::endl;
        }
        output_stream_ << std::string(50, '=') << std::endl << std::endl;
        output_stream_.flush();
        
        if (config_.real_time_display) {
            std::cout << "🎙️  Real-time Audio Transcription" << std::endl;
            std::cout << "📝 Output: " << config_.output_file << std::endl;
            std::cout << "🤖 Model: " << fs::path(config_.model_path).filename() << std::endl;
            std::cout << "🌍 Language: " << config_.language << std::endl;
            if (config_.enable_vad) {
                std::cout << "🎯 VAD: Enabled (threshold: " << config_.vad_threshold << ")" << std::endl;
            }
            std::cout << std::string(50, '-') << std::endl;
            std::cout << "Press Ctrl+C to stop" << std::endl;
            std::cout << std::string(50, '-') << std::endl;
        }
    }
    
    bool createNamedPipe() {
        unlink(pipe_path_.c_str());
        
        if (mkfifo(pipe_path_.c_str(), 0666) != 0) {
            std::cerr << "❌ Failed to create named pipe: " << pipe_path_ << std::endl;
            return false;
        }
        
        if (config_.verbose) {
            std::cout << "📡 Created pipe: " << pipe_path_ << std::endl;
        }
        
        return true;
    }
    
    bool startAudioCapture() {
        std::vector<std::string> args = {
            "./audio_capture",
            "--pipe", pipe_path_
        };
        
        if (config_.verbose) {
            std::cout << "🎤 Starting audio capture with pipe: " << pipe_path_ << std::endl;
        }
        
        capture_pid_ = fork();
        if (capture_pid_ == 0) {
            // Child process
            std::vector<char*> c_args;
            for (auto& arg : args) {
                c_args.push_back(const_cast<char*>(arg.c_str()));
            }
            c_args.push_back(nullptr);
            
            // Redirect stderr for cleaner output unless verbose
            if (!config_.verbose) {
                freopen("/dev/null", "w", stderr);
            }
            
            execv("./audio_capture", c_args.data());
            exit(1);
        } else if (capture_pid_ < 0) {
            std::cerr << "❌ Failed to start audio capture process" << std::endl;
            return false;
        }
        
        // Give capture process time to initialize and open the pipe
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Check if process is still running
        int status;
        pid_t result = waitpid(capture_pid_, &status, WNOHANG);
        if (result != 0) {
            std::cerr << "❌ Audio capture process failed to start (exit code: " << status << ")" << std::endl;
            return false;
        }
        
        return true;
    }
    
    void startTranscription() {
        start_time_ = std::chrono::steady_clock::now();
        
        transcriber_->start(pipe_path_, [this](const TranscriptionResult& result) {
            onTranscriptionResult(result);
        });
        
        if (config_.verbose) {
            std::cout << "🚀 Transcription started, listening on: " << pipe_path_ << std::endl;
        }
    }
    
    void onTranscriptionResult(const TranscriptionResult& result) {
        if (result.text.empty()) {
            return;
        }
        
        total_chunks_.fetch_add(1);
        
        // Skip very short or repetitive transcriptions
        if (result.text.length() < 3 || isRepetitiveText(result.text)) {
            if (config_.verbose) {
                std::cout << "🔇 Skipped: \"" << result.text << "\" (too short/repetitive)" << std::endl;
            }
            return;
        }
        
        transcribed_chunks_.fetch_add(1);
        
        std::string output = formatTranscription(result);
        
        // Write to console
        if (config_.real_time_display) {
            std::cout << output << std::endl;
        }
        
        // Write to file
        output_stream_ << output << std::endl;
        output_stream_.flush();
    }
    
    std::string formatTranscription(const TranscriptionResult& result) {
        std::stringstream ss;
        
        if (config_.timestamps && result.timestamp >= 0) {
            ss << "[" << formatTimestamp(result.timestamp) << "] ";
        }
        
        ss << result.text;
        
        if (config_.verbose && result.confidence > 0) {
            ss << " (conf: " << std::fixed << std::setprecision(2) << result.confidence << ")";
        }
        
        return ss.str();
    }
    
    std::string formatTimestamp(float seconds) {
        int hours = static_cast<int>(seconds) / 3600;
        int minutes = (static_cast<int>(seconds) % 3600) / 60;
        int secs = static_cast<int>(seconds) % 60;
        
        std::stringstream ss;
        ss << std::setfill('0') << std::setw(2) << hours << ":"
           << std::setfill('0') << std::setw(2) << minutes << ":"
           << std::setfill('0') << std::setw(2) << secs;
        
        return ss.str();
    }
    
    bool isRepetitiveText(const std::string& text) {
        if (text.length() < 10) return false;
        
        // Split text into words for more accurate detection
        std::vector<std::string> words;
        std::istringstream iss(text);
        std::string word;
        while (iss >> word) {
            // Clean punctuation from word for comparison
            std::string clean_word = word;
            clean_word.erase(std::remove_if(clean_word.begin(), clean_word.end(), 
                [](char c) { return std::ispunct(c); }), clean_word.end());
            
            if (!clean_word.empty()) {
                // Convert to lowercase for case-insensitive comparison
                std::transform(clean_word.begin(), clean_word.end(), clean_word.begin(), ::tolower);
                words.push_back(clean_word);
            }
        }
        
        if (words.size() < 4) return false;
        
        // Common words that shouldn't count as repetitive patterns
        std::set<std::string> common_words = {
            "the", "and", "or", "but", "in", "on", "at", "to", "for", "of", "with", "by",
            "a", "an", "is", "are", "was", "were", "be", "been", "have", "has", "had",
            "do", "does", "did", "will", "would", "could", "should", "can", "may", "might",
            "i", "you", "he", "she", "it", "we", "they", "me", "him", "her", "us", "them",
            "this", "that", "these", "those", "here", "there", "where", "when", "why", "how",
            "what", "who", "which", "so", "now", "then", "well", "okay", "ok", "yeah", "yes",
            "no", "not", "just", "like", "know", "think", "see", "look", "get", "go", "come"
        };
        
        // Check for word-level repetition patterns with adaptive thresholds
        int total_words = words.size();
        int min_pattern_length = std::max(2, total_words / 20); // Longer patterns for longer text
        int max_pattern_length = std::min(8, total_words / 3);  // Cap pattern length
        
        for (int len = min_pattern_length; len <= max_pattern_length; ++len) {
            for (size_t start = 0; start <= words.size() - len; ++start) {
                // Create pattern from consecutive words
                std::vector<std::string> pattern(words.begin() + start, words.begin() + start + len);
                
                // Skip patterns that are only common words
                bool all_common = true;
                for (const auto& pattern_word : pattern) {
                    if (common_words.find(pattern_word) == common_words.end()) {
                        all_common = false;
                        break;
                    }
                }
                if (all_common) continue;
                
                // Count exact and near-exact occurrences of this pattern
                int exact_count = 0;
                int fuzzy_count = 0;
                
                for (size_t i = 0; i <= words.size() - len; ++i) {
                    int matches = 0;
                    for (size_t j = 0; j < len; ++j) {
                        if (words[i + j] == pattern[j]) {
                            matches++;
                        }
                    }
                    
                    if (matches == len) {
                        exact_count++;
                    } else if (matches >= len * 0.7) { // 70% similarity for fuzzy match
                        fuzzy_count++;
                    }
                }
                
                // Adaptive thresholds based on text length and pattern length
                int repetition_threshold;
                if (total_words > 50) {
                    // For longer text, require more repetitions
                    repetition_threshold = std::max(5, total_words / 15);
                } else if (total_words > 20) {
                    repetition_threshold = 4;
                } else {
                    repetition_threshold = 3;
                }
                
                // Also require that repetitions make up a significant portion of the text
                double repetition_ratio = (double)(exact_count + fuzzy_count * 0.5) * len / total_words;
                
                // Flag as repetitive if:
                // 1. Too many exact repetitions, OR
                // 2. Moderate repetitions that dominate the text (>40% repetition ratio)
                if (exact_count >= repetition_threshold || 
                    (exact_count + fuzzy_count >= repetition_threshold && repetition_ratio > 0.4)) {
                    return true;
                }
            }
        }
        
        return false;
    }
    
    void printStatistics() {
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time_);
        
        std::cout << "📊 Stats: " << transcribed_chunks_.load() << " transcriptions, "
                  << total_chunks_.load() << " total chunks, "
                  << elapsed.count() << "s elapsed" << std::endl;
    }
    
    void cleanup() {
        if (transcriber_) {
            transcriber_->stop();
        }
        
        if (capture_pid_ > 0) {
            if (config_.verbose) {
                std::cout << "🛑 Stopping audio capture process..." << std::endl;
            }
            kill(capture_pid_, SIGTERM);
            waitpid(capture_pid_, nullptr, 0);
        }
        
        unlink(pipe_path_.c_str());
        
        if (output_stream_.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            
            output_stream_ << "\n" << std::string(50, '=') << std::endl;
            output_stream_ << "Session ended: " << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << std::endl;
            output_stream_ << "Total transcriptions: " << transcribed_chunks_.load() << std::endl;
            output_stream_ << std::string(50, '=') << std::endl;
            output_stream_.close();
        }
        
        if (config_.real_time_display) {
            std::cout << "\n✅ Transcription session completed" << std::endl;
            std::cout << "📊 Total transcriptions: " << transcribed_chunks_.load() << std::endl;
            std::cout << "📝 Output saved to: " << config_.output_file << std::endl;
        }
    }
};

AppConfig loadConfig(const std::string& config_file) {
    AppConfig config;
    // For now, just return default config
    // JSON config loading can be added later if needed
    return config;
}

void printUsage(const char* program) {
    std::cout << "Real-time Audio Transcription Tool\n\n";
    std::cout << "Usage: " << program << " [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -o, --output FILE       Output transcript file (default: transcript.txt)\n";
    std::cout << "  -m, --model PATH        Whisper model path (default: models/ggml-base.en.bin)\n";
    std::cout << "  -l, --language LANG     Language code (default: en)\n";
    std::cout << "  -t, --translate         Translate to English\n";
    std::cout << "  --save-audio            Save audio recordings\n";
    std::cout << "  --no-timestamps         Disable timestamps in output\n";
    std::cout << "  --no-vad                Disable voice activity detection\n";
    std::cout << "  --vad-threshold FLOAT   VAD threshold 0.0-1.0 (default: 0.6)\n";
    std::cout << "  --threads N             Number of threads (default: 4)\n";
    std::cout << "  --config FILE           Configuration file (default: config/default.json)\n";
    std::cout << "  -v, --verbose           Verbose output\n";
    std::cout << "  -h, --help              Show this help message\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << program << " -o meeting.txt\n";
    std::cout << "  " << program << " -m models/ggml-small.en.bin --save-audio\n";
    std::cout << "  " << program << " -l es --translate --vad-threshold 0.7\n";
}

int main(int argc, char** argv) {
    AppConfig config = loadConfig("config/default.json");
    
    static struct option long_options[] = {
        {"output", required_argument, 0, 'o'},
        {"model", required_argument, 0, 'm'},
        {"language", required_argument, 0, 'l'},
        {"translate", no_argument, 0, 't'},
        {"save-audio", no_argument, 0, 's'},
        {"no-timestamps", no_argument, 0, 'T'},
        {"no-vad", no_argument, 0, 'V'},
        {"vad-threshold", required_argument, 0, 1001},
        {"threads", required_argument, 0, 1002},
        {"config", required_argument, 0, 'c'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int c;
    int option_index = 0;
    
    while ((c = getopt_long(argc, argv, "o:m:l:tsTVc:vh", long_options, &option_index)) != -1) {
        switch (c) {
            case 'o':
                config.output_file = optarg;
                break;
            case 'm':
                config.model_path = optarg;
                break;
            case 'l':
                config.language = optarg;
                break;
            case 't':
                config.translate = true;
                break;
            case 's':
                config.save_audio = true;
                break;
            case 'T':
                config.timestamps = false;
                break;
            case 'V':
                config.enable_vad = false;
                break;
            case 1001:
                config.vad_threshold = std::stof(optarg);
                break;
            case 1002:
                config.threads = std::stoi(optarg);
                break;
            case 'c':
                config = loadConfig(optarg);
                break;
            case 'v':
                config.verbose = true;
                break;
            case 'h':
                printUsage(argv[0]);
                return 0;
            default:
                printUsage(argv[0]);
                return 1;
        }
    }
    
    try {
        RealTimeTranscriptionApp app(config);
        
        if (!app.initialize()) {
            return 1;
        }
        
        app.run();
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}