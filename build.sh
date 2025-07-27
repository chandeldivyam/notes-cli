#!/bin/bash
# build.sh - Optimized build script

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "🔨 Building Audio Transcriber..."

# Check dependencies
if [ ! -f "whisper.cpp/libwhisper.a" ]; then
    echo "❌ whisper.cpp not built. Run ./setup.sh first"
    exit 1
fi

# Create build directory
mkdir -p build
cd build

# Check for required tools
if ! command -v swiftc &> /dev/null; then
    echo "❌ Swift compiler not found"
    exit 1
fi

if ! command -v g++ &> /dev/null; then
    echo "❌ g++ not found"
    exit 1
fi

# Build audio capture tool
echo "🎙️ Building audio capture tool..."

# Detect architecture
ARCH=$(uname -m)
if [[ "$ARCH" == "arm64" ]]; then
    TARGET_ARCH="arm64-apple-macos13.0"
else
    TARGET_ARCH="x86_64-apple-macos13.0"
fi

echo "🏗️ Building for architecture: $ARCH"

# Build audio capture with optimizations (simplified version)
swiftc \
    -O \
    -target $TARGET_ARCH \
    -framework ScreenCaptureKit \
    -framework AVFoundation \
    ../src/audio_capture/working_audio_capture.swift \
    -o audio_capture

echo "✅ Audio capture tool built"

# Build main application
echo "🚀 Building main application..."

# Set compiler flags
CXX_FLAGS="-std=c++17 -O3 -DNDEBUG -Wall -Wextra"
CXX_FLAGS="$CXX_FLAGS -I../whisper.cpp"
CXX_FLAGS="$CXX_FLAGS -I../src"

# Add nlohmann/json
if brew --prefix nlohmann-json &> /dev/null; then
    JSON_PREFIX=$(brew --prefix nlohmann-json)
    CXX_FLAGS="$CXX_FLAGS -I$JSON_PREFIX/include"
fi

LINK_FLAGS="-framework Accelerate -pthread"
WHISPER_LIB="../whisper.cpp/libwhisper.a"

# Compile transcriber
g++ $CXX_FLAGS \
    ../src/main_fixed.cpp \
    ../src/transcriber/transcriber.cpp \
    $WHISPER_LIB \
    $LINK_FLAGS \
    -o transcriber

echo "✅ Main application built"

# Copy executables to root
cp audio_capture transcriber ../

cd ..

# Verify builds
echo "🔍 Verifying builds..."
if [ -f "transcriber" ] && [ -f "audio_capture" ]; then
    echo "✅ Build verification passed"
    
    # Test basic functionality
    echo "🧪 Testing basic functionality..."
    
    if ./transcriber --help > /dev/null 2>&1; then
        echo "✅ Transcriber help works"
    else
        echo "⚠️ Transcriber help failed"
    fi
    
    # We can't easily test audio_capture without permissions
    echo "⚠️ Audio capture requires permissions to test"
    
else
    echo "❌ Build verification failed"
    exit 1
fi

echo ""
echo "🎉 Build completed successfully!"
echo ""
echo "📁 Generated files:"
echo "  ./transcriber     - Main transcription application"
echo "  ./audio_capture   - Audio capture tool"
echo ""
echo "🚀 Ready to use:"
echo "  ./transcriber --help"
echo "  ./transcriber -o transcript.txt"
echo ""