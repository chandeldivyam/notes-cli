# 🎙️ Real-time Meeting Transcriber - Quick Start

**Ready-to-use real-time audio transcription for macOS meetings!**

## ⚡ Quick Start

```bash
# Start transcribing immediately
./transcriber -o meeting.txt

# For important meetings (better accuracy)
./transcriber --vad-threshold 0.4 -o important_meeting.txt

# Save audio recordings too
./transcriber --save-audio -o recorded_meeting.txt
```

## 🎯 Perfect for:
- ✅ **Team meetings** - Real-time notes
- ✅ **Interviews** - Automatic transcription  
- ✅ **Lectures** - Never miss important points
- ✅ **Calls** - System + microphone audio

## 📊 Example Output:
```
🎙️  Real-time Audio Transcription
📝 Output: meeting.txt
🤖 Model: ggml-base.en.bin
🌍 Language: en
🎯 VAD: Enabled (threshold: 0.4)
--------------------------------------------------
Press Ctrl+C to stop
--------------------------------------------------
[00:00:03] So, I am trying to test out
[00:00:06] if me speaking does something
[00:00:09] We need to keep the VAD monitor to a little higher
[00:00:12] so that it actually works as and when needed
```

## ⚙️ Key Settings:

| Setting | Value | Purpose |
|---------|-------|---------|
| `--vad-threshold 0.4` | Recommended | Filters background noise |
| `--vad-threshold 0.2` | Sensitive | Captures quiet speech |
| `--no-vad` | Debug mode | Transcribes everything |
| `--save-audio` | Archive | Keeps audio recordings |

## 🔧 Models Available:
- **`ggml-tiny.en.bin`** - Fastest (use with `-m models/ggml-tiny.en.bin`)
- **`ggml-base.en.bin`** - **Recommended** (default)
- **`ggml-small.en.bin`** - Best accuracy (download separately)

## 🎨 Advanced Usage:
```bash
# Fast transcription
./transcriber -m models/ggml-tiny.en.bin -o quick_notes.txt

# High accuracy
./transcriber --vad-threshold 0.3 --threads 8 -o detailed_meeting.txt

# Different language with translation
./transcriber -l es --translate -o spanish_meeting.txt

# Verbose debugging
./transcriber --verbose --vad-threshold 0.4 -o debug_meeting.txt
```

## 💡 Tips:
- **Speak clearly** into your microphone
- **Use headphones** to prevent feedback
- **Adjust VAD threshold** based on your environment:
  - Quiet room: `0.2-0.3`
  - Normal office: `0.4-0.5` 
  - Noisy environment: `0.6-0.8`

## 🛠️ Troubleshooting:
- **No transcriptions?** Try `--no-vad` first
- **Only getting "you"?** Lower VAD threshold
- **Can't stop?** Use Ctrl+C
- **Permissions needed?** Check System Settings → Privacy & Security

---
**🚀 Ready to transcribe? Just run: `./transcriber -o my_meeting.txt`**