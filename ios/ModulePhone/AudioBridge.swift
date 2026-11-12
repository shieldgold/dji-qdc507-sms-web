import AVFoundation
import Network
import ModuleCore

final class AudioBridge {
 private let queue = DispatchQueue(label: "module.phone.audio")
 private var connection: NWConnection?
 private var engine: AVAudioEngine?
 private var tapInstalled = false
 private var player: AVAudioPlayerNode?
 private var inputFramer = PCMFramer()
 private var outputFramer = PCMFramer()
 private var sends = 0
 private var scheduled = 0
 private var generation = UUID()
 private var muted = false
 var onError: ((String) -> Void)?
 var onConnected: (() -> Void)?

 static func prepareSession() throws {
  let session = AVAudioSession.sharedInstance()
  try session.setCategory(.playAndRecord, mode: .voiceChat, options: [.allowBluetoothHFP])
  try session.setPreferredSampleRate(48000)
  try session.setPreferredIOBufferDuration(0.02)
 }
 func setMuted(_ value: Bool) { queue.async { self.muted = value } }
 func speaker(_ value: Bool) throws { try AVAudioSession.sharedInstance().overrideOutputAudioPort(value ? .speaker : .none) }
 func start(host: String, ticket: AudioTicket) {
  queue.async { [weak self] in
   guard let self else { return }
   self.stopLocked()
   let id = self.generation
   let connection = NWConnection(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: ticket.port)!, using: .tcp)
   self.connection = connection
   self.queue.asyncAfter(deadline: .now() + 8) { [weak self] in
    guard let self, self.generation == id, self.engine == nil else { return }
    self.fail("音频连接超时，请检查模块连接", id: id)
   }
   connection.stateUpdateHandler = { [weak self, weak connection] state in
    guard let self, let connection, self.generation == id else { return }
    switch state {
    case .ready:
     connection.send(content: Data("PCM1 \(ticket.ticket)\n".utf8), completion: .contentProcessed { error in
      if let error { self.fail(error.localizedDescription, id: id); return }
      connection.receive(minimumIncompleteLength: 3, maximumLength: 3) { data, _, _, error in
       guard self.generation == id else { return }
       guard error == nil, data == Data("OK\n".utf8) else { self.fail("模块未接受音频连接，请重试本次通话", id: id); return }
       do { try self.startEngine(id: id); self.receive(id: id); DispatchQueue.main.async { self.onConnected?() } }
       catch { self.fail(error.localizedDescription, id: id) }
      }
     })
    case .failed(let error): self.fail(error.localizedDescription, id: id)
    default: break
    }
   }
   connection.start(queue: self.queue)
  }
 }
 func stop() { queue.async { self.stopLocked() } }
 private func stopLocked() {
  generation = UUID(); connection?.cancel(); connection = nil
  if let engine { if tapInstalled { engine.inputNode.removeTap(onBus: 0) }; engine.stop() }
  tapInstalled = false
  engine = nil; player = nil; inputFramer = PCMFramer(); outputFramer = PCMFramer(); sends = 0; scheduled = 0
 }
 private func fail(_ message: String, id: UUID) {
  guard id == generation else { return }
  stopLocked()
  DispatchQueue.main.async { self.onError?(message) }
 }
 private func startEngine(id: UUID) throws {
  let engine = AVAudioEngine()
  self.engine = engine
  try engine.inputNode.setVoiceProcessingEnabled(true)
  let micFormat = engine.inputNode.outputFormat(forBus: 0)
  guard micFormat.sampleRate > 0, micFormat.channelCount > 0,
        let wireFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 8000, channels: 1, interleaved: false),
        let converter = AVAudioConverter(from: micFormat, to: wireFormat) else { throw ModuleError.message("无法打开麦克风音频格式") }
  let player = AVAudioPlayerNode(); self.player = player
  engine.attach(player); engine.connect(player, to: engine.mainMixerNode, format: wireFormat)
  engine.inputNode.installTap(onBus: 0, bufferSize: 960, format: micFormat) { [weak self] buffer, _ in
   guard let self, let copy = AVAudioPCMBuffer(pcmFormat: micFormat, frameCapacity: buffer.frameLength) else { return }
   copy.frameLength = buffer.frameLength
   let source = UnsafeMutableAudioBufferListPointer(buffer.mutableAudioBufferList)
   let dest = UnsafeMutableAudioBufferListPointer(copy.mutableAudioBufferList)
   for i in 0..<min(source.count, dest.count) { if let s = source[i].mData, let d = dest[i].mData { memcpy(d, s, Int(source[i].mDataByteSize)) } }
   self.queue.async {
    guard self.generation == id else { return }
    let capacity = AVAudioFrameCount(Double(copy.frameLength) * 8000 / micFormat.sampleRate + 32)
    guard let converted = AVAudioPCMBuffer(pcmFormat: wireFormat, frameCapacity: capacity) else { return }
    var supplied = false; var error: NSError?
    let status = converter.convert(to: converted, error: &error) { _, state in
     if supplied { state.pointee = .noDataNow; return nil }
     supplied = true; state.pointee = .haveData; return copy
    }
    if status == .error { self.fail(error?.localizedDescription ?? "麦克风转换失败", id: id); return }
    guard let samples = converted.floatChannelData?[0] else { return }
    var data = Data(capacity: Int(converted.frameLength) * 2)
    for i in 0..<Int(converted.frameLength) {
     let value = self.muted ? Int16(0) : Int16(max(-32768, min(32767, Int(samples[i] * 32767))))
     let bits = UInt16(bitPattern: value); data.append(UInt8(bits & 255)); data.append(UInt8(bits >> 8))
    }
    for frame in self.inputFramer.append(data) {
     guard self.sends < 4 else { continue }
     self.sends += 1
     self.connection?.send(content: frame, completion: .contentProcessed { error in
      guard self.generation == id else { return }; self.sends -= 1
      if let error { self.fail(error.localizedDescription, id: id) }
     })
    }
   }
  }
  tapInstalled = true
  engine.prepare(); try engine.start(); player.play()
 }
 private func receive(id: UUID) {
  connection?.receive(minimumIncompleteLength: 1, maximumLength: 2560) { [weak self] data, _, complete, error in
   guard let self, self.generation == id else { return }
   if let data {
    for frame in self.outputFramer.append(data) {
     guard self.scheduled < 6, let player = self.player,
           let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 8000, channels: 1, interleaved: false),
           let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: 160), let samples = buffer.floatChannelData?[0] else { continue }
     let bytes = [UInt8](frame); buffer.frameLength = 160
     for i in 0..<160 { samples[i] = Float(Int16(bitPattern: UInt16(bytes[i*2]) | (UInt16(bytes[i*2+1]) << 8))) / 32768 }
     self.scheduled += 1
     player.scheduleBuffer(buffer, completionCallbackType: .dataPlayedBack) { _ in
      self.queue.async { if self.generation == id { self.scheduled -= 1 } }
     }
    }
   }
   if let error { self.fail(error.localizedDescription, id: id) }
   else if complete { self.fail("模块音频连接已结束", id: id) }
   else { self.receive(id: id) }
  }
 }
}
