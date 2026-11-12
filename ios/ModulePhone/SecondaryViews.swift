import SwiftUI

struct MessagesSheet: View {
    let api: ModuleAPI
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            ModuleWebView(api: api).navigationTitle("短信与模块").navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) { Button("完成") { dismiss() } }
                    ToolbarItem(placement: .bottomBar) { OpenSafariButton(api: api) }
                }
        }
    }
}

struct OpenSafariButton: View {
    let api: ModuleAPI
    @Environment(\.openURL) private var openURL
    @State private var busy = false
    @State private var error = ""
    @State private var showError = false
    var body: some View {
        Button {
            busy = true
            Task { @MainActor in
                defer { busy = false }
                do { openURL(try await api.browserLoginURL()) }
                catch { self.error = error.localizedDescription; showError = true }
            }
        } label: { Label(busy ? "正在打开…" : "免密打开 Safari", systemImage: "safari") }
        .disabled(busy)
        .alert("未能打开 Safari", isPresented: $showError) { Button("好", role: .cancel) {} } message: { Text(error) }
    }
}

struct NotificationSettingsView: View {
    @ObservedObject var notifications: NotificationService
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL
    @State private var testing = false
    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Label(notifications.label, systemImage: "bell")
                    if notifications.permitted { Toggle("新短信通知", isOn: $notifications.enabled) }
                    else if notifications.authorization == .denied {
                        Button("前往系统设置允许通知") { openURL(URL(string: UIApplication.openNotificationSettingsURLString)!) }
                    } else { Button("开启短信通知") { Task { await notifications.enable() } } }
                    Button("系统通知设置") { openURL(URL(string: UIApplication.openNotificationSettingsURLString)!) }
                } header: { Text("短信提醒") } footer: {
                    Text("开启后由模块发送新短信提醒，切换 App 或锁屏仍可接收。通知不展示号码、正文或验证码；首次登记不提醒历史短信。")
                }
                Section("来电提醒") {
                    Toggle("后台来电提醒", isOn: $notifications.callsEnabled)
                    Text("来电时显示系统接听界面。接听和通话需要 iPhone 保持与模块的 USB 网络连接。").font(.subheadline).foregroundStyle(.secondary)
                }
                Section("锁屏与后台") {
                    if let status = notifications.pushStatus {
                        Label(status.configured ? "模块推送服务已配置" : "等待配置推送服务", systemImage: status.configured ? "checkmark.circle" : "clock")
                        LabeledContent("来电登记", value: status.callsEnabled ? "已登记" : "未登记")
                        LabeledContent("等待发送", value: "\(status.queued)")
                        LabeledContent("本次运行苹果已接收", value: "\(status.accepted)")
                    } else { Text("请连接模块以登记后台通知。") }
                    Text("模块需要持续供电并可上网；iPhone 也需联网。登记成功不等于通知已显示，请用下方测试验证锁屏接收。重启 iPhone 后需先解锁一次。").font(.subheadline).foregroundStyle(.secondary)
                }
                Section {
                    Button(testing ? "请锁屏等待推送" : "测试后台推送") {
                        testing = true
                        Task { await notifications.testBackgroundPush(); try? await Task.sleep(for: .seconds(9)); testing = false }
                    }.disabled(!notifications.enabled || !notifications.permitted || testing || !notifications.backgroundSMSRegistered)
                    Button("仅测试本机横幅") { Task { await notifications.testNotification() } }
                        .disabled(!notifications.enabled || !notifications.permitted)
                    if !notifications.error.isEmpty { Text(notifications.error).foregroundStyle(.red) }
                } footer: { Text("后台测试由模块延迟发送，经过与真实短信相同的推送路径。本机横幅测试不经过模块。") }
            }.navigationTitle("通知").navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完成") { dismiss() } } }
                .task { await notifications.refreshPermission() }
        }
    }
}

struct NumberEntryView: View {
    @Binding var number: String
    @Environment(\.dismiss) private var dismiss
    @FocusState private var focused: Bool
    var body: some View {
        NavigationStack {
            Form {
                TextField("输入或粘贴电话号码", text: $number).keyboardType(.phonePad).textContentType(.telephoneNumber).focused($focused)
                Button("清空号码", role: .destructive) { number = "" }
            }.navigationTitle("电话号码").navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完成") { dismiss() } } }
                .task { focused = true }
        }.presentationDetents([.medium])
    }
}

struct ErrorDetailView: View {
    let message: String
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            ScrollView { Text(message).frame(maxWidth: .infinity, alignment: .leading).padding(24).textSelection(.enabled) }
                .navigationTitle("状态详情").navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完成") { dismiss() } } }
        }.presentationDetents([.medium, .large])
    }
}
