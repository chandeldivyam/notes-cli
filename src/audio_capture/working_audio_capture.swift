import Foundation
import AVFoundation
import ScreenCaptureKit

@available(macOS 13.0, *)
class WorkingAudioRecorder: NSObject {
    private let pipePath: String
    private let targetSampleRate: Int = 16000
    private let targetChannels: Int = 1
    
    private var audioEngine: AVAudioEngine!
    private var pipeHandle: FileHandle?
    private var isRecording = false
    
    init(pipePath: String) {
        self.pipePath = pipePath
        super.init()
        audioEngine = AVAudioEngine()
    }
    
    func start() async throws {
        print("🔧 Starting working audio recorder...")
        
        // Open pipe for writing
        guard let handle = FileHandle(forWritingAtPath: pipePath) else {
            throw AudioCaptureError.pipeOpenFailed
        }
        pipeHandle = handle
        print("📡 Opened pipe: \(pipePath)")
        
        // Setup audio engine with proper format handling
        try setupAudioEngine()
        
        // Start engine
        try audioEngine.start()
        isRecording = true
        
        print("✅ Audio recording started")
        print("🎤 Microphone format: \(audioEngine.inputNode.outputFormat(forBus: 0))")
        
        // Setup signal handler
        signal(SIGTERM) { _ in exit(0) }
        signal(SIGINT) { _ in exit(0) }
    }
    
    private func setupAudioEngine() throws {
        let inputNode = audioEngine.inputNode
        let inputFormat = inputNode.outputFormat(forBus: 0)
        
        print("🎵 Input format: \(inputFormat)")
        print("   Sample Rate: \(inputFormat.sampleRate)")
        print("   Channels: \(inputFormat.channelCount)")
        
        // Create target format (what whisper expects)
        guard let targetFormat = AVAudioFormat(
            standardFormatWithSampleRate: Double(targetSampleRate),
            channels: AVAudioChannelCount(targetChannels)
        ) else {
            throw AudioCaptureError.invalidFormat
        }
        
        print("🎯 Target format: \(targetFormat)")
        
        // Create converter if needed
        var processingFormat = inputFormat
        var converter: AVAudioConverter? = nil
        
        if inputFormat.sampleRate != Double(targetSampleRate) || 
           inputFormat.channelCount != AVAudioChannelCount(targetChannels) {
            
            converter = AVAudioConverter(from: inputFormat, to: targetFormat)
            processingFormat = targetFormat
            print("🔄 Created format converter")
        }
        
        // Install tap on input node
        inputNode.installTap(onBus: 0, bufferSize: 4096, format: inputFormat) { [weak self] buffer, _ in
            self?.processAudioBuffer(buffer, converter: converter, targetFormat: targetFormat)
        }
        
        print("✅ Audio tap installed")
    }
    
    private func processAudioBuffer(_ buffer: AVAudioPCMBuffer, 
                                  converter: AVAudioConverter?, 
                                  targetFormat: AVAudioFormat) {
        guard isRecording else { return }
        
        var finalBuffer = buffer
        
        // Convert format if needed
        if let converter = converter {
            guard let convertedBuffer = convertBuffer(buffer, using: converter, to: targetFormat) else {
                return
            }
            finalBuffer = convertedBuffer
        }
        
        // Calculate RMS for debugging
        let rms = calculateRMS(finalBuffer)
        if rms > 0.001 {
            print("🔊 Audio level: \(String(format: "%.6f", rms))")
        }
        
        // Convert to data and write to pipe
        if let data = bufferToData(finalBuffer) {
            pipeHandle?.write(data)
        }
    }
    
    private func convertBuffer(_ buffer: AVAudioPCMBuffer, 
                             using converter: AVAudioConverter, 
                             to targetFormat: AVAudioFormat) -> AVAudioPCMBuffer? {
        
        let frameCapacity = AVAudioFrameCount(
            Double(buffer.frameLength) * targetFormat.sampleRate / buffer.format.sampleRate
        )
        
        guard let convertedBuffer = AVAudioPCMBuffer(
            pcmFormat: targetFormat, 
            frameCapacity: frameCapacity
        ) else {
            return nil
        }
        
        var error: NSError?
        let status = converter.convert(to: convertedBuffer, error: &error) { _, outStatus in
            outStatus.pointee = .haveData
            return buffer
        }
        
        if status == .error {
            print("❌ Conversion error: \(error?.localizedDescription ?? "Unknown")")
            return nil
        }
        
        return convertedBuffer
    }
    
    private func calculateRMS(_ buffer: AVAudioPCMBuffer) -> Float {
        guard let channelData = buffer.floatChannelData else { return 0 }
        
        let frameLength = Int(buffer.frameLength)
        let channelCount = Int(buffer.format.channelCount)
        
        var rms: Float = 0
        for channel in 0..<channelCount {
            let data = channelData[channel]
            for frame in 0..<frameLength {
                let sample = data[frame]
                rms += sample * sample
            }
        }
        
        return sqrt(rms / Float(frameLength * channelCount))
    }
    
    private func bufferToData(_ buffer: AVAudioPCMBuffer) -> Data? {
        guard let channelData = buffer.floatChannelData else { return nil }
        
        let frameLength = Int(buffer.frameLength)
        let channelCount = Int(buffer.format.channelCount)
        let totalSamples = frameLength * channelCount
        
        var audioData = Data(capacity: totalSamples * MemoryLayout<Float>.size)
        
        // Interleave channels
        for frame in 0..<frameLength {
            for channel in 0..<channelCount {
                var sample = channelData[channel][frame]
                audioData.append(withUnsafeBytes(of: &sample) { Data($0) })
            }
        }
        
        return audioData
    }
}

enum AudioCaptureError: Error {
    case permissionDenied
    case pipeOpenFailed
    case invalidFormat
    case noDisplay
}

// Simple command line parsing
func parseArguments(_ args: [String]) -> (pipePath: String?, help: Bool) {
    var pipePath: String?
    var help = false
    
    var i = 1
    while i < args.count {
        let arg = args[i]
        
        if arg == "--help" || arg == "-h" {
            help = true
        } else if arg == "--pipe" && i + 1 < args.count {
            pipePath = args[i + 1]
            i += 1
        }
        i += 1
    }
    
    return (pipePath, help)
}

func printUsage() {
    print("Usage: working_audio_capture --pipe <pipe_path>")
    print("Options:")
    print("  --pipe <path>    Named pipe path for audio output")
    print("  --help, -h       Show this help message")
}

// Main execution
if #available(macOS 13.0, *) {
    let args = CommandLine.arguments
    let (pipePath, help) = parseArguments(args)
    
    if help {
        printUsage()
        exit(0)
    }
    
    guard let pipePath = pipePath else {
        print("❌ Error: --pipe argument required")
        printUsage()
        exit(1)
    }
    
    let recorder = WorkingAudioRecorder(pipePath: pipePath)
    
    Task {
        do {
            try await recorder.start()
        } catch {
            print("❌ Error: \(error)")
            exit(1)
        }
    }
    
    RunLoop.main.run()
} else {
    print("❌ macOS 13.0+ required")
    exit(1)
}