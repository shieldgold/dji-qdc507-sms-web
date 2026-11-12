import AVFoundation
import Network
import XCTest
import ModuleCore
@testable import ModulePhone

final class AudioBridgeTests: XCTestCase {
 @MainActor func testMicrophoneSustainsPCMForTwelveSeconds() async throws {
  #if targetEnvironment(simulator)
  throw XCTSkip("Requires real iPhone audio hardware")
  #endif
  guard ProcessInfo.processInfo.environment["MODULE_AUDIO_HARDWARE_TEST"] == "1" else {
   throw XCTSkip("Opt in to real microphone test in the ModulePhone scheme")
  }
  let allowed = await AVAudioApplication.requestRecordPermission()
  XCTAssertTrue(allowed)
  guard allowed else { return }
  let ready = expectation(description: "Local PCM listener")
  let connected = expectation(description: "Audio engine running")
  let streamed = expectation(description: "12 seconds of microphone PCM received")
  let peer = try SilentPCMPeer(ready: ready, streamed: streamed)
  let bridge = AudioBridge()
  defer { bridge.stop(); peer.stop(); try? AVAudioSession.sharedInstance().setActive(false) }
  peer.start()
  await fulfillment(of: [ready], timeout: 3)
  try AudioBridge.prepareSession()
  try AVAudioSession.sharedInstance().setActive(true)
  bridge.onConnected = { connected.fulfill() }
  bridge.onError = { message in XCTFail("Audio failed: \(message)") }
  let ticket = try JSONDecoder().decode(AudioTicket.self, from: Data("{\"ticket\":\"test\",\"port\":\(peer.port)}".utf8))
  bridge.start(host: "127.0.0.1", ticket: ticket)
  await fulfillment(of: [connected, streamed], timeout: 13.5)
  let bytes = peer.byteCount
  print("AUDIO_TEST microphone_bytes=\(bytes)")
  XCTAssertGreaterThanOrEqual(bytes, 192_000)
 }
}

// Discard microphone samples; send silence back at 8 kHz. Never record audio.
private final class SilentPCMPeer {
 private let queue = DispatchQueue(label: "module.audio.test.peer")
 private let listener: NWListener
 private var connection: NWConnection?
 private var timer: DispatchSourceTimer?
 private var bytes = 0
 private var reported = false
 private let ready: XCTestExpectation
 private let streamed: XCTestExpectation
 var port: UInt16 { listener.port!.rawValue }
 var byteCount: Int { queue.sync { bytes } }
 init(ready: XCTestExpectation, streamed: XCTestExpectation) throws {
  self.ready = ready; self.streamed = streamed
  listener = try NWListener(using: .tcp, on: .any)
 }
 func start() {
  listener.stateUpdateHandler = { [weak self] state in
   if case .ready = state { self?.ready.fulfill() }
  }
  listener.newConnectionHandler = { [weak self] connection in
   guard let self else { return }
   self.connection = connection
   connection.start(queue: self.queue)
   connection.receive(minimumIncompleteLength: 10, maximumLength: 10) { _, _, _, error in
    guard error == nil else { return }
    connection.send(content: Data("OK\n".utf8), completion: .contentProcessed { _ in })
    self.read()
    let timer = DispatchSource.makeTimerSource(queue: self.queue)
    timer.schedule(deadline: .now(), repeating: .milliseconds(20))
    timer.setEventHandler { connection.send(content: Data(count: 320), completion: .contentProcessed { _ in }) }
    self.timer = timer; timer.resume()
   }
  }
  listener.start(queue: queue)
 }
 private func read() {
  connection?.receive(minimumIncompleteLength: 1, maximumLength: 3200) { [weak self] data, _, complete, error in
   guard let self else { return }
   self.bytes += data?.count ?? 0
   if self.bytes >= 192_000, !self.reported { self.reported = true; self.streamed.fulfill() }
   if error == nil, !complete { self.read() }
  }
 }
 func stop() { queue.sync { timer?.cancel(); connection?.cancel(); listener.cancel() } }
}
