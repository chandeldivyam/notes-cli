# 🎙️ Real-time Audio Transcription Engine for macOS

A high-performance CLI tool that captures microphone and system audio simultaneously and provides real-time transcription using whisper.cpp with advanced optimizations for meeting transcription.

## ✨ Key Features

- **🚀 Real-time Processing**: Optimized for live transcription with minimal latency
- **🎯 Dual Audio Capture**: Simultaneous microphone and system audio recording
- **🧠 Smart VAD**: Voice Activity Detection to reduce CPU usage and improve accuracy  
- **🔄 Streaming Architecture**: Ring buffers and lock-free queues for smooth audio flow
- **📝 Live Output**: Real-time transcript display with timestamps
- **💾 Session Management**: Automatic session logging with metadata
- **🎛️ Configurable**: Flexible settings via JSON config and CLI options
- **⚡ Metal Acceleration**: Leverages Apple Silicon for faster inference

## 🔧 Requirements

- **macOS 13.0+** (for ScreenCaptureKit)
- **Xcode Command Line Tools**
- **Permissions**: Microphone and Screen Recording access
- **Hardware**: Apple Silicon recommended for best performance

## 🚀 Quick Start

```bash
# Clone and setup
git clone <repository-url>
cd cli-notes
./setup.sh

# Start transcribing immediately  
./transcriber -o meeting.txt

# Advanced usage
./transcriber --save-audio --vad-threshold 0.7 -o important_meeting.txt
```

## 📦 Installation

### Automated Setup
```bash
./setup.sh
```

This will:
- ✅ Verify macOS version and dependencies
- 📦 Install required tools (Homebrew, CMake, etc.)
- 📥 Download and build whisper.cpp
- 🤖 Download optimized Whisper models
- 🔨 Build the complete system
- 🔗 Create system symlinks

### Manual Build
```bash
# Install dependencies
brew install cmake pkg-config nlohmann-json

# Clone whisper.cpp
git clone https://github.com/ggml-org/whisper.cpp.git
cd whisper.cpp && git checkout v1.5.4
make libwhisper.a CFLAGS="-O3 -DNDEBUG"
cd ..

# Build transcriber
make all
```

## 🎯 Usage

### Basic Commands
```bash
# Default transcription
./transcriber

# Specify output file
./transcriber -o meeting_notes.txt

# Save audio recordings
./transcriber --save-audio -o interview.txt

# Different language with translation
./transcriber -l es --translate -o spanish_meeting.txt

# High accuracy mode (slower)
./transcriber -m models/ggml-small.en.bin --threads 8
```

### CLI Options
```
Options:
  -o, --output FILE       Output transcript file (default: transcript.txt)
  -m, --model PATH        Whisper model path (default: models/ggml-base.en.bin)
  -l, --language LANG     Language code (default: en)
  -t, --translate         Translate to English
      --save-audio        Save audio recordings
      --no-timestamps     Disable timestamps in output
      --no-vad            Disable voice activity detection
      --vad-threshold N   VAD threshold 0.0-1.0 (default: 0.6)
      --threads N         Number of threads (default: 4)
      --config FILE       Configuration file
  -v, --verbose           Verbose output
  -h, --help              Show help message
```

## ⚙️ Configuration

### Default Config (`config/default.json`)
```json
{
  "audio": {
    "sample_rate": 16000,
    "channels": 1,
    "chunk_duration_ms": 3000,
    "overlap_ms": 500,
    "vad_threshold": 0.6,
    "enable_vad": true
  },
  "transcription": {
    "model": "models/ggml-base.en.bin",
    "language": "en",
    "translate": false,
    "threads": 4,
    "temperature": 0.0
  },
  "output": {
    "timestamps": true,
    "real_time": true,
    "file": "transcript.txt"
  }
}
```

### Model Selection Guide
| Model | Size | Speed | Accuracy | Use Case |
|-------|------|-------|----------|----------|
| `tiny.en` | 39MB | ⚡⚡⚡ | ⭐⭐ | Testing, very fast hardware |
| `base.en` | 142MB | ⚡⚡ | ⭐⭐⭐ | **Recommended for real-time** |
| `small.en` | 466MB | ⚡ | ⭐⭐⭐⭐ | High accuracy, some latency |
| `medium.en` | 1.5GB | 🐌 | ⭐⭐⭐⭐⭐ | Offline processing only |

## 📊 Performance Optimization

### Real-time Tips
1. **Use base.en model** - Best speed/accuracy balance
2. **Enable VAD** - Reduces unnecessary processing
3. **Optimize threads** - Match your CPU core count
4. **Lower VAD threshold** - For noisy environments
5. **Use headphones** - Prevents audio feedback

### System Optimization
```bash
# Check CPU usage
top -pid $(pgrep transcriber)

# Monitor audio latency
./transcriber --verbose

# Profile performance
./transcriber --config config/performance.json
```

## 📋 Output Format

### Live Console Output
```
🎙️  Real-time Audio Transcription
📝 Output: meeting.txt
🤖 Model: ggml-base.en.bin
🌍 Language: en
🎯 VAD: Enabled (threshold: 0.6)
--------------------------------------------------
Press Ctrl+C to stop
--------------------------------------------------
[00:00:03] Hello everyone, welcome to today's meeting.
[00:00:08] Let's start with the quarterly review.
[00:00:15] The numbers look really promising this quarter.
```

### Saved Transcript (`meeting.txt`)
```
==================================================
🎙️  TRANSCRIPTION SESSION
==================================================
Started: 2024-01-15 14:30:25
Model: models/ggml-base.en.bin
Language: en
Sample Rate: 16000Hz
VAD: Enabled
VAD Threshold: 0.6
==================================================

[00:00:03] Hello everyone, welcome to today's meeting.
[00:00:08] Let's start with the quarterly review.
[00:00:15] The numbers look really promising this quarter.
[00:00:22] Can everyone see the presentation on their screens?

==================================================
Session ended: 2024-01-15 15:15:42
Total transcriptions: 87
==================================================
```

## 🔧 Advanced Features

### Voice Activity Detection
```bash
# Sensitive VAD (picks up quiet speech)
./transcriber --vad-threshold 0.3

# Conservative VAD (only clear speech)  
./transcriber --vad-threshold 0.8

# Disable VAD (transcribe everything)
./transcriber --no-vad
```

### Audio Recording
```bash
# Save all audio for later analysis
./transcriber --save-audio

# Files saved to recordings/
# recording_2024-01-15T14:30:25.wav
```

### Multi-language Support
```bash
# Spanish with English translation
./transcriber -l es --translate

# French transcription
./transcriber -l fr -m models/ggml-base.bin

# Auto-detect language (slower)
./transcriber -l auto -m models/ggml-base.bin
```

## 🛠️ Development

### Build System
```bash
make help           # Show all targets
make setup          # Initial setup
make all            # Build everything
make test           # Run tests
make clean          # Clean builds
make dev-build      # Debug build
```

### Architecture
```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Audio Capture │    │   Ring Buffer    │    │   Transcriber   │
│   (Swift)       │───▶│   (C++)          │───▶│   (whisper.cpp) │
│                 │    │                  │    │                 │
│ • Microphone    │    │ • Lock-free      │    │ • VAD           │
│ • System Audio  │    │ • Low latency    │    │ • Streaming     │
│ • ScreenCapture │    │ • Thread-safe    │    │ • Metal GPU     │
└─────────────────┘    └──────────────────┘    └─────────────────┘
        │                        │                        │
        ▼                        ▼                        ▼
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Named Pipe    │    │   Audio Queue    │    │   Text Output   │
│                 │    │                  │    │                 │
│ • IPC           │    │ • Buffering      │    │ • Console       │
│ • Streaming     │    │ • Overlap        │    │ • File          │
└─────────────────┘    └──────────────────┘    └─────────────────┘
```

## 🚨 Troubleshooting

### Permission Issues
```bash
# Grant microphone permission
# System Settings > Privacy & Security > Microphone

# Grant screen recording permission  
# System Settings > Privacy & Security > Screen Recording
```

### Audio Problems
```bash
# Check audio devices
./transcriber --verbose

# Test with lower quality
./transcriber -m models/ggml-tiny.en.bin

# Disable VAD if having issues
./transcriber --no-vad
```

### Performance Issues
```bash
# Reduce threads on older machines
./transcriber --threads 2

# Use smaller model
./transcriber -m models/ggml-tiny.en.bin

# Check system resources
top -pid $(pgrep transcriber)
```

### Build Issues
```bash
# Clean and rebuild
make clean && make setup && make all

# Check dependencies
brew doctor
xcode-select --install

# Manual whisper.cpp build
cd whisper.cpp && make clean && make libwhisper.a
```

## 📈 Use Cases

### 🏢 Business Meetings
```bash
./transcriber -o "$(date +%Y%m%d)_team_meeting.txt" --save-audio
```

### 🎓 Lectures & Education  
```bash
./transcriber -m models/ggml-small.en.bin -o lecture_notes.txt
```

### 🎤 Interviews
```bash
./transcriber --save-audio --vad-threshold 0.4 -o interview.txt
```

### 🌍 Multi-language Events
```bash
./transcriber -l auto --translate -m models/ggml-base.bin
```

## 📚 Technical Details

### Audio Pipeline
- **Sample Rate**: 16kHz (optimized for speech)
- **Channels**: Mono (reduces processing load)
- **Buffer Size**: 3-second chunks with 500ms overlap
- **Latency**: <1 second end-to-end

### Transcription Engine
- **Model**: Whisper.cpp with Metal acceleration
- **VAD**: Energy-based with adaptive thresholding
- **Streaming**: Overlapping windows for smooth output
- **Threading**: Separate audio and transcription threads

### Memory Management
- **Ring Buffers**: Lock-free circular buffers
- **Audio Queue**: Bounded queue with backpressure
- **Model Loading**: Single model instance, reused state

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

## 📄 License

MIT License - see LICENSE file for details

## 🙏 Acknowledgments

- [whisper.cpp](https://github.com/ggml-org/whisper.cpp) - Fast Whisper inference
- [OpenAI Whisper](https://github.com/openai/whisper) - Original model
- Apple ScreenCaptureKit - macOS audio capture API

---

**Ready to start transcribing?** 🚀
```bash
./setup.sh && ./transcriber -o my_first_transcript.txt
```