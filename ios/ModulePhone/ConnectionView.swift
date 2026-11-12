import SwiftUI
import WebKit

struct ConnectionView: View {
    @ObservedObject var phone: CallManager
    @ObservedObject var saved: SavedConnection
    @State private var password = ""
    @State private var pin = ""
    @State private var remember = true
    @Environment(\.dismiss) private var dismiss
    init(phone: CallManager) { self.phone = phone; self.saved = phone.savedConnection }
    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("http://192.168.225.1:8787", text: $phone.address).keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
                    if !saved.cardSuffix.isEmpty { LabeledContent("当前 SIM", value: "尾号 " + saved.cardSuffix) }
                    SecureField(saved.hasPassword ? "已保存，留空自动登录" : "模块网页访问密码", text: $password).textContentType(.password)
                    Toggle("保存访问密码，自动登录", isOn: $remember).onChange(of: remember) { _, enabled in if !enabled { saved.forgetPassword() } }
                    Button {
                        Task {
                            await phone.connect(password: password, remember: remember); password = ""
                            if phone.loggedIn && phone.connected && saved.simState == "READY" { dismiss() }
                        }
                    } label: {
                        HStack { Text(phone.busy ? "连接中…" : "连接模块"); Spacer(); if phone.busy { ProgressView() } }
                    }.disabled(phone.busy)
                } header: { Text("连接") } footer: { Text("凭据只保存在这台 iPhone 的系统钥匙串。同卡自动登录，检测到换卡就清除旧凭据，要求重新输入。") }
                Section {
                    if !saved.simState.isEmpty { LabeledContent("卡片状态", value: saved.simState) }
                    if saved.hasPIN {
                        Label(saved.pinPaused ? "自动解锁已暂停" : "已保存 · 同卡自动解锁", systemImage: saved.pinPaused ? "pause.circle" : "lock.open")
                        Button("删除保存的 PIN", role: .destructive) { saved.forgetPIN() }
                    }
                    SecureField("输入 SIM PIN（4–8 位数字）", text: $pin).keyboardType(.numberPad)
                    Button(saved.simState == "SIM PIN" ? "保存并解锁一次" : "保存 PIN，开启同卡自动解锁") {
                        Task { await saved.savePIN(pin, api: phone.api); pin = ""; await phone.refresh() }
                    }.disabled(!phone.loggedIn || saved.busy || pin.isEmpty || saved.blocked)
                } header: { Text("SIM PIN") } footer: {
                    Text("访问密码与 SIM PIN 不同。只有实时核对卡号一致时才提交 PIN；解锁失败或结果不明会停止自动尝试。保存时若 SIM 已解锁，将在下次锁定时验证 PIN。")
                }
                if !saved.notice.isEmpty { Section { Text(saved.notice).font(.subheadline) } }
                if !phone.error.isEmpty { Section { Text(phone.error).foregroundStyle(.red) } }
                if saved.hasPassword || saved.hasPIN { Section { Button("删除保存的密码和 PIN", role: .destructive) { saved.forget() } } }
                Section { Text("首次使用还没有访问密码时，请到首页的“短信与模块”设置。") }
            }.navigationTitle("连接设置").navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完成") { dismiss() } } }
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
