import SwiftUI

struct MessagesSheet: View {
    let api: ModuleAPI
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            ModuleWebView(api: api).navigationTitle("短信与模块").navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完成") { dismiss() } } }
        }
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
                    Text("App 在前台并连接模块时，新短信会显示通知和角标。通知中不展示号码、正文或验证码。首次同步历史短信不会提醒。")
                }
                Section("来电提醒") {
                    Label("前台来电使用系统接听界面", systemImage: "phone.badge.waveform")
                    Text("保持 App 前台并连接模块，来电会调用系统通话界面，可接听或拒接。已接通电话可以继续使用系统通话控制。").font(.subheadline).foregroundStyle(.secondary)
                }
                Section("锁屏与后台") {
                    Label("后台新短信／来电唤醒尚未接入", systemImage: "moon.zzz")
                    Text("切换 App 或锁屏后，系统可能暂停连接，无法保证发现新的短信或来电。重新打开后会同步新增短信。开启系统通知权限不会解除这一限制。").font(.subheadline).foregroundStyle(.secondary)
                }
                Section {
                    Button(testing ? "3 秒后显示测试通知" : "测试通知") {
                        testing = true
                        Task { await notifications.testNotification(); try? await Task.sleep(for: .seconds(4)); testing = false }
                    }.disabled(!notifications.enabled || !notifications.permitted || testing)
                    if !notifications.error.isEmpty { Text(notifications.error).foregroundStyle(.red) }
                } footer: { Text("测试只检查系统通知显示，不代表后台接收已接通。") }
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
