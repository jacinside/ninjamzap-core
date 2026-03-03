import Foundation

class VorbisEncoder {
    private var tempPath: String?
    private var buffer: [Float] = []

    func prepareRecording() {
        let tempDir = NSTemporaryDirectory()
        let tempFile = UUID().uuidString + ".ogg"
        let fullPath = (tempDir as NSString).appendingPathComponent(tempFile)
        self.tempPath = fullPath
        buffer.removeAll()
        Logger.debug("📝 VorbisEncoder: Preparing at \(fullPath)")
    }

    func process(samples: UnsafePointer<Float>, numFrames: Int) {
        let chunk = Array(UnsafeBufferPointer(start: samples, count: numFrames))
        buffer.append(contentsOf: chunk)
    }

    func finishRecording() -> Data? {
        guard let path = tempPath else { return nil }

//        let result = encode_to_ogg(buffer, Int32(buffer.count), 44100, path)
//        guard result == 0 else {
//            Logger.error("❌ Vorbis encoding failed")
//            return nil
//        }

        let data = try? Data(contentsOf: URL(fileURLWithPath: path))
        try? FileManager.default.removeItem(atPath: path)
        Logger.debug("✅ VorbisEncoder: File ready and deleted")
        return data
    }
  
    func reset() {
      buffer.removeAll()
      tempPath = nil
      Logger.debug("🗑️ VorbisEncoder: Reset")
    }
}
