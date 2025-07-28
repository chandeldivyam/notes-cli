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
    
    // ScreenCaptureKit for system audio
    private var stream: SCStream?
    private var mainMixer: AVAudioMixerNode!
    private var sessionStarted = false
    private let sessionQueue = DispatchQueue(label: "audio.session")
    private var systemAudioBuffer: [Float] = []
    private let bufferLock = NSLock()
    
    init(pipePath: String) {
        self.pipePath = pipePath
        super.init()
        audioEngine = AVAudioEngine()
    }
    
    func start() async throws {
        print("🔧 Starting dual audio recorder (mic + system)...")
        
        // Request permissions
        await requestPermissions()
        
        // Open pipe for writing
        guard let handle = FileHandle(forWritingAtPath: pipePath) else {
            throw AudioCaptureError.pipeOpenFailed
        }
        pipeHandle = handle
        print("📡 Opened pipe: \(pipePath)")
        
        // Setup system audio capture first
        try await setupSystemAudio()
        
        // Setup audio engine with both mic and system audio
        try setupAudioEngine()
        
        // Start system audio capture
        try await stream?.startCapture()
        print("🔊 System audio capture started")
        
        // Start audio engine for microphone
        try audioEngine.start()
        isRecording = true
        
        print("✅ Dual audio recording started")
        print("🎤 Microphone format: \(audioEngine.inputNode.outputFormat(forBus: 0))")
        
        // Setup signal handler
        signal(SIGTERM) { _ in exit(0) }
        signal(SIGINT) { _ in exit(0) }
    }
    
    private func setupAudioEngine() throws {
        let inputNode = audioEngine.inputNode
        let inputFormat = inputNode.outputFormat(forBus: 0)
        
        print("🎵 Mic input format: \(inputFormat)")
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
        
        // Create converter if needed for microphone
        var converter: AVAudioConverter? = nil
        
        if inputFormat.sampleRate != Double(targetSampleRate) || 
           inputFormat.channelCount != AVAudioChannelCount(targetChannels) {
            
            converter = AVAudioConverter(from: inputFormat, to: targetFormat)
            print("🔄 Created format converter for microphone")
        }
        
        // Install tap on input node to get microphone audio and mix with system audio
        inputNode.installTap(onBus: 0, bufferSize: 4096, format: inputFormat) { [weak self] buffer, _ in
            self?.processMicrophoneBuffer(buffer, converter: converter, targetFormat: targetFormat)
        }
        
        print("✅ Dual audio engine setup complete")
    }
    
    private func processMicrophoneBuffer(_ buffer: AVAudioPCMBuffer, 
                                       converter: AVAudioConverter?, 
                                       targetFormat: AVAudioFormat) {
        guard isRecording else { return }
        
        var micBuffer = buffer
        
        // Convert format if needed
        if let converter = converter {
            guard let convertedBuffer = convertBuffer(buffer, using: converter, to: targetFormat) else {
                return
            }
            micBuffer = convertedBuffer
        }
        
        // Mix microphone audio with system audio
        let mixedBuffer = mixAudioBuffers(micBuffer: micBuffer, targetFormat: targetFormat)
        
        // Calculate RMS for debugging
        let rms = calculateRMS(mixedBuffer)
        if rms > 0.001 {
            print("🔊 Mixed audio level: \(String(format: "%.6f", rms))")
        }
        
        // Convert to data and write to pipe
        if let data = bufferToData(mixedBuffer) {
            pipeHandle?.write(data)
        }
    }
    
    private func mixAudioBuffers(micBuffer: AVAudioPCMBuffer, targetFormat: AVAudioFormat) -> AVAudioPCMBuffer {
        guard let mixedBuffer = AVAudioPCMBuffer(pcmFormat: targetFormat, frameCapacity: micBuffer.frameCapacity) else {
            return micBuffer
        }
        
        mixedBuffer.frameLength = micBuffer.frameLength
        
        guard let micChannelData = micBuffer.floatChannelData,
              let mixedChannelData = mixedBuffer.floatChannelData else {
            return micBuffer
        }
        
        let frameLength = Int(micBuffer.frameLength)
        let channelCount = Int(targetFormat.channelCount)
        
        bufferLock.lock()
        defer { bufferLock.unlock() }
        
        // Copy microphone data and mix with system audio
        for channel in 0..<channelCount {
            for frame in 0..<frameLength {
                var sample = micChannelData[channel][frame] * 0.7 // Mic at 70% volume
                
                // Add system audio if available
                let systemIndex = frame * channelCount + channel
                if systemIndex < systemAudioBuffer.count {
                    sample += systemAudioBuffer[systemIndex] * 0.5 // System at 50% volume
                }
                
                mixedChannelData[channel][frame] = sample
            }
        }
        
        // Clear used system audio samples
        let totalSamples = frameLength * channelCount
        if totalSamples <= systemAudioBuffer.count {
            systemAudioBuffer.removeFirst(totalSamples)
        } else {
            systemAudioBuffer.removeAll()
        }
        
        return mixedBuffer
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
    
    private func requestPermissions() async {
        _ = await AVCaptureDevice.requestAccess(for: .audio)
    }
    
    private func setupSystemAudio() async throws {
        // Get display for system audio capture
        guard let content = try? await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true),
              let display = content.displays.first else {
            throw AudioCaptureError.noDisplay
        }
        
        // Configure stream for system audio
        let streamConfig = SCStreamConfiguration()
        streamConfig.capturesAudio = true
        streamConfig.sampleRate = targetSampleRate
        streamConfig.channelCount = targetChannels
        
        let filter = SCContentFilter(display: display, excludingApplications: [], exceptingWindows: [])
        stream = SCStream(filter: filter, configuration: streamConfig, delegate: nil)
        
        try stream?.addStreamOutput(self, type: .audio, sampleHandlerQueue: .global())
        
        print("🔧 System audio capture configured")
    }
    
    private func startSessionIfNeeded() {
        sessionQueue.async { [weak self] in
            guard let self = self, !self.sessionStarted else { return }
            self.sessionStarted = true
            print("🎵 Audio session started")
        }
    }
    
    private func createSampleBuffer(from audioBuffer: AVAudioPCMBuffer) -> CMSampleBuffer? {
        let format = audioBuffer.format
        var asbd = format.streamDescription.pointee
        var formatDescription: CMFormatDescription?
        
        guard CMAudioFormatDescriptionCreate(
            allocator: kCFAllocatorDefault,
            asbd: &asbd,
            layoutSize: 0,
            layout: nil,
            magicCookieSize: 0,
            magicCookie: nil,
            extensions: nil,
            formatDescriptionOut: &formatDescription
        ) == noErr else { return nil }
        
        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: Int32(asbd.mSampleRate)),
            presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()),
            decodeTimeStamp: .invalid
        )
        
        var sampleBuffer: CMSampleBuffer?
        
        guard CMSampleBufferCreate(
            allocator: kCFAllocatorDefault,
            dataBuffer: nil,
            dataReady: false,
            makeDataReadyCallback: nil,
            refcon: nil,
            formatDescription: formatDescription,
            sampleCount: CMItemCount(audioBuffer.frameLength),
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timing,
            sampleSizeEntryCount: 0,
            sampleSizeArray: nil,
            sampleBufferOut: &sampleBuffer
        ) == noErr else { return nil }
        
        guard let buffer = sampleBuffer else { return nil }
        
        guard CMSampleBufferSetDataBufferFromAudioBufferList(
            buffer,
            blockBufferAllocator: kCFAllocatorDefault,
            blockBufferMemoryAllocator: kCFAllocatorDefault,
            flags: 0,
            bufferList: audioBuffer.mutableAudioBufferList
        ) == noErr else { return nil }
        
        return buffer
    }
    
    func stop() {
        print("\nStopping dual audio recording...")
        
        isRecording = false
        
        // Stop system audio
        stream?.stopCapture()
        
        // Stop audio engine
        audioEngine.stop()
        mainMixer?.removeTap(onBus: 0)
        
        // Close pipe
        pipeHandle?.closeFile()
        
        print("🛑 Dual audio recording stopped")
    }
}

// MARK: - SCStreamOutput
extension WorkingAudioRecorder: SCStreamOutput {
    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .audio, isRecording else { return }
        
        // Start session on first sample if needed
        if !sessionStarted {
            startSessionIfNeeded()
        }
        
        // Convert CMSampleBuffer to audio samples and add to buffer
        guard let audioSamples = convertSampleBufferToFloatArray(sampleBuffer) else {
            return
        }
        
        // Add system audio samples to buffer for mixing
        bufferLock.lock()
        systemAudioBuffer.append(contentsOf: audioSamples)
        
        // Limit buffer size to prevent memory issues
        let maxBufferSize = targetSampleRate * targetChannels * 2 // 2 seconds max
        if systemAudioBuffer.count > maxBufferSize {
            let removeCount = systemAudioBuffer.count - maxBufferSize
            systemAudioBuffer.removeFirst(removeCount)
        }
        bufferLock.unlock()
        
        // Calculate RMS for debugging
        let rms = calculateRMSFromSamples(audioSamples)
        if rms > 0.001 {
            print("🔊 System audio level: \(String(format: "%.6f", rms))")
        }
    }
    
    private func convertSampleBufferToPCMBuffer(_ sampleBuffer: CMSampleBuffer) -> AVAudioPCMBuffer? {
        guard let formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer) else {
            return nil
        }
        
        let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(formatDescription)
        guard let asbd = asbd else { return nil }
        
        guard let format = AVAudioFormat(streamDescription: asbd) else {
            return nil
        }
        
        let frameCount = CMSampleBufferGetNumSamples(sampleBuffer)
        guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frameCount)) else {
            return nil
        }
        
        buffer.frameLength = AVAudioFrameCount(frameCount)
        
        // Copy audio data
        guard let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else {
            return nil
        }
        
        var dataPointer: UnsafeMutablePointer<Int8>?
        var lengthAtOffset: Int = 0
        
        guard CMBlockBufferGetDataPointer(blockBuffer, atOffset: 0, lengthAtOffsetOut: &lengthAtOffset, totalLengthOut: nil, dataPointerOut: &dataPointer) == noErr else {
            return nil
        }
        
        guard let channelData = buffer.floatChannelData else {
            return nil
        }
        
        let channelCount = Int(format.channelCount)
        let frameLength = Int(buffer.frameLength)
        
        // Convert data based on format
        if format.commonFormat == .pcmFormatFloat32 {
            if let dataPointer = dataPointer {
                let floatPtr = dataPointer.withMemoryRebound(to: Float.self, capacity: frameLength * channelCount) { ptr in
                    return ptr
                }
                for channel in 0..<channelCount {
                    for frame in 0..<frameLength {
                        channelData[channel][frame] = floatPtr[frame * channelCount + channel]
                    }
                }
            }
        }
        
        return buffer
    }
    
    private func convertSampleBufferToFloatArray(_ sampleBuffer: CMSampleBuffer) -> [Float]? {
        guard let formatDescription = CMSampleBufferGetFormatDescription(sampleBuffer) else {
            return nil
        }
        
        let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(formatDescription)
        guard let asbd = asbd else { return nil }
        
        guard let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else {
            return nil
        }
        
        var dataPointer: UnsafeMutablePointer<Int8>?
        var lengthAtOffset: Int = 0
        
        guard CMBlockBufferGetDataPointer(blockBuffer, atOffset: 0, lengthAtOffsetOut: &lengthAtOffset, totalLengthOut: nil, dataPointerOut: &dataPointer) == noErr else {
            return nil
        }
        
        let frameCount = CMSampleBufferGetNumSamples(sampleBuffer)
        let channelCount = Int(asbd.pointee.mChannelsPerFrame)
        
        var samples: [Float] = []
        
        // Convert based on format
        if asbd.pointee.mFormatID == kAudioFormatLinearPCM {
            if asbd.pointee.mBitsPerChannel == 32 && (asbd.pointee.mFormatFlags & kAudioFormatFlagIsFloat) != 0 {
                // Float32 format
                if let dataPointer = dataPointer {
                    let floatPtr = dataPointer.withMemoryRebound(to: Float.self, capacity: frameCount * channelCount) { ptr in
                        return ptr
                    }
                    samples = Array(UnsafeBufferPointer(start: floatPtr, count: frameCount * channelCount))
                }
            } else if asbd.pointee.mBitsPerChannel == 16 {
                // Int16 format - convert to Float
                if let dataPointer = dataPointer {
                    let int16Ptr = dataPointer.withMemoryRebound(to: Int16.self, capacity: frameCount * channelCount) { ptr in
                        return ptr
                    }
                    samples = (0..<(frameCount * channelCount)).map { index in
                        Float(int16Ptr[index]) / Float(Int16.max)
                    }
                }
            }
        }
        
        // Convert to target format (mono, 16kHz) if needed
        if channelCount > 1 || asbd.pointee.mSampleRate != Double(targetSampleRate) {
            samples = resampleAndMixDown(samples, 
                                       fromSampleRate: Double(asbd.pointee.mSampleRate),
                                       fromChannels: channelCount,
                                       toSampleRate: Double(targetSampleRate),
                                       toChannels: targetChannels)
        }
        
        return samples
    }
    
    private func resampleAndMixDown(_ samples: [Float], 
                                  fromSampleRate: Double, 
                                  fromChannels: Int,
                                  toSampleRate: Double, 
                                  toChannels: Int) -> [Float] {
        var result = samples
        
        // Mix down channels if needed
        if fromChannels > toChannels && toChannels == 1 {
            var monoSamples: [Float] = []
            let frameCount = samples.count / fromChannels
            
            for frame in 0..<frameCount {
                var sum: Float = 0
                for channel in 0..<fromChannels {
                    sum += samples[frame * fromChannels + channel]
                }
                monoSamples.append(sum / Float(fromChannels))
            }
            result = monoSamples
        }
        
        // Resample if needed (simple linear interpolation)
        if fromSampleRate != toSampleRate {
            let ratio = fromSampleRate / toSampleRate
            let newCount = Int(Double(result.count) / ratio)
            var resampledSamples: [Float] = []
            
            for i in 0..<newCount {
                let sourceIndex = Double(i) * ratio
                let lowIndex = Int(sourceIndex)
                let highIndex = min(lowIndex + 1, result.count - 1)
                let fraction = Float(sourceIndex - Double(lowIndex))
                
                if lowIndex < result.count {
                    let interpolated = result[lowIndex] * (1.0 - fraction) + 
                                     (highIndex < result.count ? result[highIndex] * fraction : 0)
                    resampledSamples.append(interpolated)
                }
            }
            result = resampledSamples
        }
        
        return result
    }
    
    private func calculateRMSFromSamples(_ samples: [Float]) -> Float {
        guard !samples.isEmpty else { return 0 }
        
        let sumOfSquares = samples.reduce(0) { $0 + $1 * $1 }
        return sqrt(sumOfSquares / Float(samples.count))
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