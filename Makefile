# Makefile for Real-time Audio Transcriber

CXX = g++
SWIFT = swiftc
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -I./whisper.cpp -I./src
SWIFTFLAGS = -O -framework ScreenCaptureKit -framework AVFoundation
LDFLAGS = -framework Accelerate -pthread

# Add nlohmann/json if available via Homebrew
JSON_PREFIX := $(shell brew --prefix nlohmann-json 2>/dev/null)
ifneq ($(JSON_PREFIX),)
    CXXFLAGS += -I$(JSON_PREFIX)/include
endif

WHISPER_LIB = whisper.cpp/libwhisper.a
TARGET = transcriber
AUDIO_CAPTURE = audio_capture

SRCS = src/main.cpp \
       src/transcriber/transcriber.cpp

OBJS = $(SRCS:.cpp=.o)

.PHONY: all clean setup install test help models

all: $(TARGET) $(AUDIO_CAPTURE)

$(TARGET): $(OBJS) $(WHISPER_LIB)
	@echo "🔗 Linking $(TARGET)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Built $(TARGET)"

$(AUDIO_CAPTURE): src/audio_capture/audio_capture.swift
	@echo "🎙️ Building $(AUDIO_CAPTURE)..."
	@mkdir -p build
	@cd build && \
	echo '// swift-tools-version:5.5\nimport PackageDescription\nlet package = Package(name: "AudioCapture", platforms: [.macOS(.v13)], dependencies: [.package(url: "https://github.com/apple/swift-argument-parser", from: "1.0.0")], targets: [.executableTarget(name: "AudioCapture", dependencies: [.product(name: "ArgumentParser", package: "swift-argument-parser")], path: "../src/audio_capture")])' > Package.swift && \
	swift package resolve && \
	$(SWIFT) $(SWIFTFLAGS) \
		-parse-as-library \
		-whole-module-optimization \
		-target x86_64-apple-macos13.0 \
		-I .build/checkouts/swift-argument-parser/Sources/ArgumentParser/include \
		../src/audio_capture/audio_capture.swift \
		-o ../$(AUDIO_CAPTURE)
	@echo "✅ Built $(AUDIO_CAPTURE)"

$(WHISPER_LIB):
	@echo "📦 Building whisper.cpp..."
	@if [ ! -d "whisper.cpp" ]; then \
		echo "❌ whisper.cpp not found. Run 'make setup' first"; \
		exit 1; \
	fi
	cd whisper.cpp && make clean && make libwhisper.a CFLAGS="-O3 -DNDEBUG -std=c11 -fPIC" CXXFLAGS="-O3 -DNDEBUG -std=c++11 -fPIC"

%.o: %.cpp
	@echo "🔨 Compiling $<..."
	$(CXX) $(CXXFLAGS) -c -o $@ $<

setup:
	@echo "🚀 Running setup..."
	./setup.sh

models:
	@echo "📥 Downloading models..."
	./scripts/download_models.sh

clean:
	@echo "🧹 Cleaning..."
	rm -f $(OBJS) $(TARGET) $(AUDIO_CAPTURE)
	rm -rf build
	@if [ -d "whisper.cpp" ]; then cd whisper.cpp && make clean; fi

install: all
	@echo "📦 Installing..."
	mkdir -p /usr/local/bin
	cp $(TARGET) /usr/local/bin/audio-transcriber
	cp $(AUDIO_CAPTURE) /usr/local/bin/
	@echo "✅ Installed to /usr/local/bin/"

test: all
	@echo "🧪 Running tests..."
	@echo "Testing transcriber help..."
	./$(TARGET) --help > /dev/null
	@echo "✅ Basic tests passed"
	@echo "⚠️ Audio capture requires permissions for full testing"

# Development targets
dev-build: 
	@echo "🔧 Development build (debug)..."
	$(MAKE) CXXFLAGS="-std=c++17 -g -Wall -Wextra -I./whisper.cpp -I./src" all

format:
	@echo "🎨 Formatting code..."
	@find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i || echo "clang-format not available"

lint:
	@echo "🔍 Linting code..."
	@find src -name "*.cpp" -o -name "*.h" | xargs cppcheck --enable=all --suppress=missingIncludeSystem || echo "cppcheck not available"

help:
	@echo "📖 Available targets:"
	@echo "  all         - Build everything"
	@echo "  setup       - Run initial setup"
	@echo "  models      - Download Whisper models"
	@echo "  clean       - Clean build files"
	@echo "  install     - Install to system"
	@echo "  test        - Run basic tests"
	@echo "  dev-build   - Build with debug info"
	@echo "  format      - Format source code"
	@echo "  lint        - Lint source code"
	@echo "  help        - Show this help"
	@echo ""
	@echo "🚀 Quick start:"
	@echo "  make setup && make all"
	@echo "  ./transcriber -o test.txt"