import AVFoundation
import CallKit
import SwiftUI
import ModuleCore

@MainActor final class CallManager: NSObject, ObservableObject, @preconcurrency CXProviderDelegate {
 @Published var address = "http://192.168.225.1:8787"
 @Published var number = ""
 @Published var connected = false
 @Published var loggedIn = false
 @Published var busy = false
 @Published var error = ""
 @Published var status: ModuleCallState?
 @Published var microphoneConnected = false
 @Published var muted = false
 @Published var speaker = false
 @Published var localPending = false
 private(set) var api = ModuleAPI(endpoint: try! ModuleEndpoint("http://192.168.225.1:8787"))
 private let controller = CXCallController()
 private var provider: CXProvider!
 private let audio = AudioBridge()
 private var polling: Task<Void, Never>?
 private var refreshing = false
 private var endpointGeneration = UUID()
 private var uuid: UUID?
 private var currentID: String?
 private var connectedReported = false
 private var activated = false
 private var audioStarted = false
 private var audioSessionID: UUID?
 private var outgoingRequest = ""
 private var outgoingAt = Date.distantPast
 private var endPending = false
 private var cancelUncertainDial = false
 var current: VoiceCall? { status?.telephone.calls.first }
 var hasCall: Bool { current != nil || localPending || status?.telephone.pending == true }
 override init() {
  super.init()
  let config = CXProviderConfiguration()
  config.supportsVideo = false; config.maximumCallsPerCallGroup = 1; config.maximumCallGroups = 1; config.supportedHandleTypes = [.phoneNumber]
  provider = CXProvider(configuration: config); provider.setDelegate(self, queue: .main)

 }
 func connect(password: String) async {
  guard !busy else { return }
  busy = true; error = ""; defer { busy = false }
  do {
   guard !hasCall else { throw ModuleError.message("请先结束当前电话") }
   let endpoint = try ModuleEndpoint(address)
   endpointGeneration = UUID(); polling?.cancel(); polling = nil; loggedIn = false; connected = false; status = nil
   api = ModuleAPI(endpoint: endpoint)
   if !password.isEmpty { try await api.login(password: password) } else { await api.importWebSession() }
   await api.shareSessionWithWebView()
   let data = try await api.state(); status = data; loggedIn = true; connected = true
   _ = try? await api.request("api/time", body: ["unix_ms": Int64(Date().timeIntervalSince1970 * 1000)])
   startPolling()
  } catch { self.error = error.localizedDescription; connected = false }
 }
 func startPolling() {
  guard loggedIn, polling == nil else { return }
  polling = Task { [weak self] in
   while !Task.isCancelled { await self?.refresh(); try? await Task.sleep(nanoseconds: 1_000_000_000) }
  }
 }
 func setForeground(_ foreground: Bool) {
  if foreground { startPolling() }
  else if !hasCall { polling?.cancel(); polling = nil }
 }
 func refresh() async {
  guard loggedIn, !refreshing else { return }; refreshing = true
  let generation = endpointGeneration
  defer { refreshing = false }
  do {
   let next = try await api.state()
   guard generation == endpointGeneration else { return }
   status = next; connected = true
   if let message = next.telephone.error, !message.isEmpty { error = message; return }
   if let call = next.telephone.calls.first {
    localPending = false; currentID = call.id
    if uuid == nil {
     let id = UUID(); uuid = id
     let update = CXCallUpdate(); update.remoteHandle = CXHandle(type: .phoneNumber, value: call.number.isEmpty ? "未知号码" : call.number)
     update.hasVideo = false; update.supportsDTMF = true; update.supportsHolding = false; update.supportsGrouping = false; update.supportsUngrouping = false
     if call.isIncoming {
      do { try await provider.reportNewIncomingCall(with: id, update: update) }
      catch { self.error = error.localizedDescription }
      guard uuid == id else { return }
     }
    }
    if call.state == "active", !connectedReported, let id = uuid {
     connectedReported = true
     if call.direction == "out" { provider.reportOutgoingCall(with: id, connectedAt: Date()) }
    }
    if cancelUncertainDial, call.requestID == outgoingRequest { endPending = true }
    if endPending { await performHangup(); return }
    await startAudioIfNeeded()
   } else if let id = uuid, !next.telephone.pending, !localPending || Date().timeIntervalSince(outgoingAt) > 20 {
    provider.reportCall(with: id, endedAt: Date(), reason: .remoteEnded); clearCall()
   }
  } catch {
   // Leaving the foreground cancels polling. It is not a module failure and
   // must not overwrite the preceding call/audio error with "request cancelled".
   guard !(error is CancellationError), (error as? URLError)?.code != .cancelled else { return }
   if generation == endpointGeneration { connected = false; self.error = error.localizedDescription }
  }
 }
 func dial() async {
  error = ""
  guard connected, !hasCall, let n = PhoneNumber.normalize(number) else { error = "请连接模块并输入有效电话号码"; return }
  guard status?.audio.available == true else { error = "模块音频尚未准备好"; return }
  let allowed = await AVAudioApplication.requestRecordPermission()
  guard allowed else { error = "请在系统设置中允许“模块电话”使用麦克风"; return }
  guard connected, !hasCall else { error = "电话状态已变化，请重试"; return }
  do { try AudioBridge.prepareSession() } catch { self.error = error.localizedDescription; return }
  outgoingRequest = UUID().uuidString; outgoingAt = Date(); localPending = true
  let id = UUID(); uuid = id
  do { try await controller.request(CXTransaction(action: CXStartCallAction(call: id, handle: CXHandle(type: .phoneNumber, value: n)))) }
  catch { clearCall(); self.error = error.localizedDescription }
 }
 func answer() async {
  guard let id = uuid else { return }
  let allowed = await AVAudioApplication.requestRecordPermission()
  guard allowed else { error = "需要麦克风权限才能通话"; return }
  do { try AudioBridge.prepareSession(); try await controller.request(CXTransaction(action: CXAnswerCallAction(call: id))) }
  catch { self.error = error.localizedDescription }
 }
 func endCall() {
  guard let id = uuid else { return }
  Task { do { try await controller.request(CXTransaction(action: CXEndCallAction(call: id))) } catch { self.error = error.localizedDescription; await performHangup() } }
 }
 private func performHangup() async {
  endPending = true
  guard let id = currentID else { return }
  // Closing the call-bound audio stream also asks the module to end this call,
  // even if the HTTP session has expired or the control response is lost.
  audioSessionID = nil; audio.stop(); microphoneConnected = false
  do { _ = try await api.request("api/calls/hangup", body: ["id": id]); endPending = false; audioSessionID = nil; audio.stop(); microphoneConnected = false }
  catch { self.error = "挂断结果未确认，请保持连接并刷新：\(error.localizedDescription)" }
 }
 func sendDigit(_ digit: String) {
  guard let id = currentID else { return }
  Task { do { _ = try await api.request("api/calls/dtmf", body: ["id": id, "digit": digit]) } catch { self.error = error.localizedDescription } }
 }
 func toggleMute() { muted.toggle(); audio.setMuted(muted) }
 func toggleSpeaker() {
  do { try audio.speaker(!speaker); speaker.toggle() } catch { self.error = error.localizedDescription }
 }
 private func startAudioIfNeeded() async {
  guard activated, !audioStarted, let id = uuid, currentID != nil else { return }; audioStarted = true
  do {
   let ticket = try await api.ticket()
   guard activated, uuid == id else { return }
   let audioID = UUID(); audioSessionID = audioID
   audio.onError = { [weak self] text in
    guard let self, self.uuid == id, self.audioSessionID == audioID else { return }
    self.microphoneConnected = false; self.error = "音频中断：\(text)"; self.endCall()
   }
   audio.onConnected = { [weak self] in
    guard let self, self.uuid == id, self.audioSessionID == audioID else { return }
    self.microphoneConnected = true
   }
   audio.start(host: api.endpoint.url.host!, ticket: ticket)
  } catch { if uuid == id { audioStarted = false; self.error = error.localizedDescription } }
 }
 private func clearCall() {
  audioSessionID = nil; audio.stop(); audio.setMuted(false); uuid = nil; currentID = nil; connectedReported = false; activated = false; audioStarted = false; localPending = false; microphoneConnected = false; endPending = false; cancelUncertainDial = false; muted = false; speaker = false
 }
 func providerDidReset(_ provider: CXProvider) { audioSessionID = nil; audio.stop(); activated = false; audioStarted = false; Task { await performHangup(); await refresh() } }
 func provider(_ provider: CXProvider, perform action: CXStartCallAction) {
  Task {
   do {
    _ = try await api.request("api/calls/dial", body: ["number": action.handle.value, "request_id": outgoingRequest])
    provider.reportOutgoingCall(with: action.callUUID, startedConnectingAt: Date()); action.fulfill(); await refresh()
   } catch { self.error = error.localizedDescription; cancelUncertainDial = true; action.fail(); await refresh(); if current == nil && status?.telephone.pending != true { clearCall() } }
  }
 }
 func provider(_ provider: CXProvider, perform action: CXAnswerCallAction) {
  Task {
   guard action.callUUID == uuid, let id = currentID else { action.fail(); return }
   do { guard await AVAudioApplication.requestRecordPermission() else { throw ModuleError.message("需要麦克风权限才能通话") }; guard action.callUUID == uuid, currentID == id else { action.fail(); return }; try AudioBridge.prepareSession(); _ = try await api.request("api/calls/answer", body: ["id": id]); action.fulfill(); await refresh() }
   catch { self.error = error.localizedDescription; action.fail() }
  }
 }
 func provider(_ provider: CXProvider, perform action: CXEndCallAction) {
  Task { guard action.callUUID == uuid else { action.fail(); return }; await performHangup(); if endPending { action.fail() } else { action.fulfill() } }
 }
 func provider(_ provider: CXProvider, perform action: CXSetMutedCallAction) { muted = action.isMuted; audio.setMuted(muted); action.fulfill() }
 func provider(_ provider: CXProvider, perform action: CXPlayDTMFCallAction) { action.digits.forEach { sendDigit(String($0)) }; action.fulfill() }
 func provider(_ provider: CXProvider, didActivate audioSession: AVAudioSession) { activated = true; Task { await startAudioIfNeeded() } }
 func provider(_ provider: CXProvider, didDeactivate audioSession: AVAudioSession) { activated = false; audioSessionID = nil; audio.stop(); audioStarted = false; microphoneConnected = false }
}
