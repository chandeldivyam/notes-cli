#!/bin/bash
# scripts/download_models.sh - Download optimized Whisper models

set -e

MODELS_DIR="models"
mkdir -p "$MODELS_DIR"

echo "📥 Downloading Whisper models for real-time transcription..."

# Base URLs
HUGGINGFACE_BASE="https://huggingface.co/ggerganov/whisper.cpp/resolve/main"

# Model configurations (simplified for better compatibility)
TINY_MODEL="ggml-tiny.en.bin"
BASE_MODEL="ggml-base.en.bin"
SMALL_MODEL="ggml-small.en.bin"
MEDIUM_MODEL="ggml-medium.en.bin"
LARGE_V3_TURBO_MODEL="ggml-large-v3-turbo.bin"
LARGE_V3_TURBO_Q8_MODEL="ggml-large-v3-turbo-q8_0.bin"

# Check available disk space
AVAILABLE_SPACE=$(df -m "$MODELS_DIR" | tail -1 | awk '{print $4}')
echo "💾 Available disk space: ${AVAILABLE_SPACE}MB"

download_model() {
    local model_name="$1"
    local description="$2"
    local model_path="$MODELS_DIR/$model_name"
    
    if [ -f "$model_path" ]; then
        echo "✅ $model_name already exists"
        return 0
    fi
    
    echo "📥 Downloading $model_name - $description"
    
    # Download with progress bar and resume capability
    curl -L --progress-bar \
         --retry 3 \
         --retry-delay 5 \
         --continue-at - \
         "$HUGGINGFACE_BASE/$model_name" \
         -o "$model_path.tmp"
    
    # Verify download completed successfully
    if [ $? -eq 0 ]; then
        mv "$model_path.tmp" "$model_path"
        echo "✅ Downloaded $model_name"
        
        # Show file size
        local size=$(du -h "$model_path" | cut -f1)
        echo "📏 Size: $size"
    else
        echo "❌ Failed to download $model_name"
        rm -f "$model_path.tmp"
        return 1
    fi
}

# Download recommended models
echo "🎯 Downloading recommended models for real-time usage..."

# Always download tiny for testing
download_model "$TINY_MODEL" "Fastest, lowest accuracy (39MB)"

# Always download base (recommended)
download_model "$BASE_MODEL" "Recommended for real-time (142MB)"

# Ask user about additional models
echo ""
echo "📋 Additional models available:"
echo "  small.en  - Better accuracy but slower (466MB)"
echo "  medium.en - High accuracy but high latency (1.5GB)"
echo ""

read -p "❓ Download small.en model? [y/N]: " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    download_model "$SMALL_MODEL" "Better accuracy, slower (466MB)"
fi

read -p "❓ Download medium.en model? [y/N]: " -n 1 -r  
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    download_model "$MEDIUM_MODEL" "High accuracy, high latency (1.5GB)"
fi

read -p "❓ Download large-v3-turbo model (latest, 4x faster than large-v3)? [y/N]: " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    download_model "$LARGE_V3_TURBO_MODEL" "Latest model, 4x faster than large-v3 (1.6GB)"
fi

read -p "❓ Download large-v3-turbo-q8_0 model (8-bit quantized, smaller)? [y/N]: " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    download_model "$LARGE_V3_TURBO_Q8_MODEL" "8-bit quantized turbo model (874MB)"
fi

# Download multilingual base if user wants other languages
echo ""
read -p "❓ Download multilingual base model for other languages? [y/N]: " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    download_model "ggml-base.bin" "Multilingual base model (142MB)"
fi

echo ""
echo "📊 Model recommendations:"
echo "  🚀 Real-time: Use ggml-tiny.en.bin or ggml-base.en.bin"
echo "  ⚖️  Balanced: Use ggml-base.en.bin (default)"
echo "  🎯 Accuracy: Use ggml-small.en.bin"
echo "  🔬 Best quality: Use ggml-medium.en.bin (not real-time)"
echo "  ⚡ Latest/Fastest: Use ggml-large-v3-turbo-q8_0.bin (balanced size/quality)"
echo "  🏆 Best overall: Use ggml-large-v3-turbo.bin (latest OpenAI model)"
echo ""

echo "✅ Model download completed!"
echo ""
echo "📁 Downloaded models:"
ls -lh "$MODELS_DIR"/*.bin 2>/dev/null || echo "No models found"
echo ""
echo "💡 The default model (ggml-base.en.bin) provides the best balance"
echo "   between speed and accuracy for real-time transcription."
echo ""