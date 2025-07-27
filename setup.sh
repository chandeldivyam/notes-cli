#!/bin/bash
# setup.sh - Optimized setup script for real-time audio transcription

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "🚀 Setting up Real-time Audio Transcriber..."

# Check macOS version
MACOS_VERSION=$(sw_vers -productVersion | cut -d. -f1)
if [[ $MACOS_VERSION -lt 13 ]]; then
    echo "❌ Error: macOS 13.0+ required for ScreenCaptureKit"
    echo "💡 Current version: $(sw_vers -productVersion)"
    exit 1
fi

echo "✅ macOS version check passed"

# Check if Xcode Command Line Tools are installed
if ! command -v xcode-select &> /dev/null || ! xcode-select -p &> /dev/null; then
    echo "📦 Installing Xcode Command Line Tools..."
    xcode-select --install
    echo "⏳ Please complete Xcode Command Line Tools installation and run this script again"
    exit 1
fi

echo "✅ Xcode Command Line Tools found"

# Install Homebrew if not installed
if ! command -v brew &> /dev/null; then
    echo "📦 Installing Homebrew..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    
    # Add Homebrew to PATH
    if [[ -f "/opt/homebrew/bin/brew" ]]; then
        echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zshrc
        eval "$(/opt/homebrew/bin/brew shellenv)"
    fi
fi

echo "✅ Homebrew ready"

# Install dependencies
echo "📦 Installing build dependencies..."
brew install cmake pkg-config nlohmann-json

# Note: swift-argument-parser is not available via Homebrew
# It will be downloaded automatically during the Swift build process

echo "✅ Dependencies installed"

# Clone and build whisper.cpp
echo "📥 Setting up whisper.cpp..."
if [ ! -d "whisper.cpp" ]; then
    echo "📥 Cloning whisper.cpp..."
    git clone https://github.com/ggml-org/whisper.cpp.git
    cd whisper.cpp
    git checkout v1.5.4  # Use stable version
    cd ..
else
    echo "📁 whisper.cpp already exists"
fi

# Build whisper.cpp with optimizations
echo "🔨 Building whisper.cpp..."
cd whisper.cpp
make clean 2>/dev/null || true
make libwhisper.a CFLAGS="-O3 -DNDEBUG -std=c11 -fPIC" CXXFLAGS="-O3 -DNDEBUG -std=c++11 -fPIC"
cd ..

echo "✅ whisper.cpp built successfully"

# Create necessary directories
echo "📁 Creating directories..."
mkdir -p models recordings build

# Download models
echo "🤖 Downloading Whisper models..."
bash scripts/download_models.sh

# Build the project
echo "🔨 Building transcription engine..."
bash build.sh

# Set up permissions
echo "🔐 Setting up permissions..."
chmod +x transcriber audio_capture
chmod +x scripts/*.sh

# Create symlinks for easier access
if [ ! -L "/usr/local/bin/audio-transcriber" ]; then
    echo "🔗 Creating system-wide symlink..."
    sudo ln -sf "$SCRIPT_DIR/transcriber" /usr/local/bin/audio-transcriber 2>/dev/null || {
        echo "⚠️ Could not create system symlink (requires sudo)"
        echo "💡 You can run ./transcriber from this directory"
    }
fi

echo ""
echo "✅ Setup complete!"
echo ""
echo "📖 Usage:"
echo "  ./transcriber --help              # Show help"
echo "  ./transcriber -o meeting.txt      # Basic usage"
echo "  ./transcriber --save-audio        # Save audio files"
echo ""
echo "📋 Next steps:"
echo "  1. Grant microphone permission when prompted"
echo "  2. Grant screen recording permission in System Settings"
echo "  3. Run: ./transcriber -o test.txt"
echo ""
echo "🔧 Advanced:"
echo "  Edit config/default.json to customize settings"
echo "  Use different models from models/ directory"
echo ""