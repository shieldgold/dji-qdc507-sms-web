import SwiftUI
import WebKit

@main struct ModulePhoneApp: App {
 @StateObject private var phone = CallManager()
 @Environment(\.scenePhase) private var phase
 var body: some Scene {
  WindowGroup {
   ContentView(phone: phone)
    .onChange(of: phase) { _, value in phone.setForeground(value == .active) }
  }
 }
}

struct ContentView: View {
 @ObservedObject var phone: CallManager
 @State private var connectionSheet = false
 @State private var showKeypad = false
 @State private var selection = 0
 let keys = [["1","2","3"],["4","5","6"],["7","8","9"],["*","0","#"]]
 var body: some View {
  TabView(selection: $selection) {
   NavigationStack {
    ScrollView {
     VStack(spacing: 24) {
      HStack(spacing: 8) {
       Circle().fill(phone.connected ? Color.green : Color.orange).frame(width: 7, height: 7)
       Text(phone.connected ? "模块已连接" : "等待连接模块").font(.subheadline)
       Spacer()
       Button("连接设置") { connectionSheet = true }.font(.subheadline)
      }
      if !phone.error.isEmpty {
       Text(phone.error).font(.callout).foregroundStyle(.red).padding(14).frame(maxWidth: .infinity, alignment: .leading).background(Color.red.opacity(0.06), in: RoundedRectangle(cornerRadius: 14))
      }
      if phone.hasCall {
       VStack(spacing: 12) {
        Text(phone.current?.stateLabel ?? "正在发起电话").font(.headline).foregroundStyle(.secondary)
        Text(phone.current?.number.isEmpty == false ? phone.current!.number : phone.number).font(.system(size: 32, weight: .medium, design: .rounded)).minimumScaleFactor(0.6).lineLimit(1)
        Label(audioLabel, systemImage: phone.microphoneConnected ? "waveform" : "waveform.slash").font(.footnote).foregroundStyle(.secondary)
       }.padding(.vertical, 26)
       if phone.current?.isIncoming == true {
        Button { Task { await phone.answer() } } label: { Label("接听", systemImage: "phone.fill").font(.title3.bold()).frame(maxWidth: .infinity).padding() }.buttonStyle(.borderedProminent).tint(.green)
       } else {
        HStack(spacing: 24) {
         roundAction(phone.muted ? "取消静音" : "静音", symbol: phone.muted ? "mic.slash.fill" : "mic.fill", active: phone.muted) { phone.toggleMute() }
         roundAction("键盘", symbol: "circle.grid.3x3.fill", active: showKeypad) { showKeypad.toggle() }
         roundAction("扬声器", symbol: "speaker.wave.2.fill", active: phone.speaker) { phone.toggleSpeaker() }
        }
       }
       if showKeypad { keypad(inCall: true) }
       Button { phone.endCall() } label: { Label(phone.current?.isIncoming == true ? "拒接" : "挂断", systemImage: "phone.down.fill").font(.title3.bold()).frame(maxWidth: .infinity).padding() }.buttonStyle(.borderedProminent).tint(.red)
      } else {
       VStack(spacing: 10) {
        Image(systemName: "simcard").font(.system(size: 30)).foregroundStyle(.teal)
        Text("使用模块 SIM 拨打电话").font(.subheadline).foregroundStyle(.secondary)
        TextField("输入电话号码", text: $phone.number).font(.system(size: 30, weight: .regular, design: .rounded)).multilineTextAlignment(.center).keyboardType(.phonePad).textContentType(.telephoneNumber).padding(.vertical, 8).accessibilityIdentifier("dialNumber")
       }.padding(.top, 10)
       keypad(inCall: false)
       HStack {
        Spacer()
        Button { Task { await phone.dial() } } label: { Image(systemName: "phone.fill").font(.system(size: 27)).frame(width: 76, height: 76).foregroundStyle(.white).background(phone.connected && phone.status?.audio.available == true ? Color.green : Color.gray, in: Circle()) }.disabled(!phone.connected || phone.status?.audio.available != true).accessibilityLabel("拨打电话")
        Spacer()
       }
       Text(phone.status?.audio.available == true ? "声音通过模块直连传输" : "连接模块后检查语音状态").font(.footnote).foregroundStyle(.secondary)
      }
      Text("来电提醒目前需要 App 保持前台；已接通电话可使用系统通话界面。").font(.caption).foregroundStyle(.secondary).frame(maxWidth: .infinity, alignment: .leading).padding(.top, 6)
     }.padding(.horizontal, 24).padding(.bottom, 24)
    }.background(Color(.systemGroupedBackground)).navigationTitle("模块电话").toolbar { ToolbarItem(placement: .topBarTrailing) { Button { Task { await phone.refresh() } } label: { Image(systemName: "arrow.clockwise") }.disabled(!phone.loggedIn) } }
   }.tabItem { Label("电话", systemImage: "phone") }.tag(0)
   NavigationStack {
    ModuleWebView(api: phone.api).id(phone.loggedIn).navigationTitle("短信与模块").navigationBarTitleDisplayMode(.inline)
   }.tabItem { Label("短信与模块", systemImage: "bubble.left.and.text.bubble.right") }.tag(1)
  }.tint(.teal)
   .sheet(isPresented: $connectionSheet) { ConnectionView(phone: phone) }
   .task { await phone.connect(password: ""); if !phone.loggedIn { connectionSheet = true } }
 }
 var audioLabel: String {
  if let audio = phone.status?.audio, audio.state == "streaming", audio.downBytes > 0 { return "语音数据传输中" }
  return phone.microphoneConnected ? "音频已连接，等待电话接通" : "正在连接音频"
 }
 func roundAction(_ title: String, symbol: String, active: Bool, action: @escaping () -> Void) -> some View {
  Button(action: action) { VStack(spacing: 8) { Image(systemName: symbol).font(.title2).frame(width: 64, height: 64).background(active ? Color.teal.opacity(0.18) : Color(.secondarySystemGroupedBackground), in: Circle()); Text(title).font(.caption) } }.foregroundStyle(.primary)
 }
 func keypad(inCall: Bool) -> some View {
  VStack(spacing: 14) {
   ForEach(keys, id: \.self) { row in
    HStack(spacing: 26) {
     ForEach(row, id: \.self) { key in
      Button { if inCall { phone.sendDigit(key) } else { phone.number += key } } label: { Text(key).font(.system(size: 30, weight: .regular, design: .rounded)).frame(width: 70, height: 62).background(Color(.secondarySystemGroupedBackground), in: Circle()) }.foregroundStyle(.primary).accessibilityLabel(key)
     }
    }
   }
   if !inCall {
    HStack { Button("+") { phone.number += "+" }.frame(width: 70); Spacer(); Button { if !phone.number.isEmpty { phone.number.removeLast() } } label: { Image(systemName: "delete.left").frame(width: 70) }.accessibilityLabel("删除一位") }.frame(width: 262)
   }
  }
 }
}

struct ConnectionView: View {
 @ObservedObject var phone: CallManager
 @State private var password = ""
 @Environment(\.dismiss) var dismiss
 var body: some View {
  NavigationStack {
   Form {
    Section { Text("将 iPhone 连接到模块的 USB 网络，然后输入原有的模块网页访问密码。") }
    Section("模块地址") { TextField("http://192.168.225.1:8787", text: $phone.address).keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled() }
    Section("访问密码") { SecureField("模块网页密码", text: $password).textContentType(.password) }
    if !phone.error.isEmpty { Section { Text(phone.error).foregroundStyle(.red) } }
    Section {
     Button { Task { await phone.connect(password: password); password = ""; if phone.loggedIn && phone.connected { dismiss() } } } label: { HStack { Text(phone.busy ? "连接中…" : "连接模块"); Spacer(); if phone.busy { ProgressView() } } }.disabled(phone.busy)
    }
    Section { Text("密码只用于登录模块。初次使用请先在“短信与模块”页面设置访问密码。") }
   }.navigationTitle("连接模块").navigationBarTitleDisplayMode(.inline).toolbar { ToolbarItem(placement: .cancellationAction) { Button("完成") { dismiss() } } }
  }
 }
}

struct ModuleWebView: UIViewRepresentable {
 let api: ModuleAPI
 func makeUIView(context: Context) -> WKWebView {
  let view = WKWebView()
  Task { @MainActor in
   for cookie in HTTPCookieStorage.shared.cookies(for: api.endpoint.url) ?? [] { await view.configuration.websiteDataStore.httpCookieStore.setCookie(cookie) }
   view.load(URLRequest(url: api.endpoint.url))
  }
  return view
 }
 func updateUIView(_ view: WKWebView, context: Context) {}
}
