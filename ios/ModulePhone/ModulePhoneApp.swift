import SwiftUI
import ModuleCore

@main struct ModulePhoneApp: App {
    @UIApplicationDelegateAdaptor(PushAppDelegate.self) private var appDelegate
    @StateObject private var phone = CallManager.shared
    @StateObject private var dashboard = DashboardModel()
    @StateObject private var notifications = NotificationService.shared
    @Environment(\.scenePhase) private var phase
    var body: some Scene {
        WindowGroup {
            ContentView(phone: phone, dashboard: dashboard, notifications: notifications)
                .onChange(of: phase) { _, value in
                    // Permission sheets and CallKit temporarily make the app inactive.
                    if value == .active { phone.setForeground(true) }
                    else if value == .background { phone.setForeground(false) }
                }
        }
    }
}

private enum DashboardSheet: String, Identifiable {
    case connection, messages, notifications, number, error
    var id: String { rawValue }
}

struct ContentView: View {
    @ObservedObject var phone: CallManager
    @ObservedObject var dashboard: DashboardModel
    @ObservedObject var notifications: NotificationService
    @Environment(\.scenePhase) private var phase
    @Environment(\.dynamicTypeSize) private var typeSize
    @State private var sheet: DashboardSheet?
    @State private var showKeypad = false
    private let brandAccent = Color(red: 0.82, green: 0.29, blue: 0.04)
    private let keys = [["1", "2", "3"], ["4", "5", "6"], ["7", "8", "9"], ["*", "0", "#"]]
    private let letters = ["2":"ABC", "3":"DEF", "4":"GHI", "5":"JKL", "6":"MNO", "7":"PQRS", "8":"TUV", "9":"WXYZ", "0":"+"]
    private var pollingKey: String { "\(phone.loggedIn)-\(phone.api.endpoint.url)-\(phase == .active)" }

    var body: some View {
        GeometryReader { geometry in
            Group {
                if typeSize.isAccessibilitySize {
                    ScrollView { home(compact: false).padding(.vertical, 8) }
                } else { home(compact: geometry.size.height < 680) }
            }
            .frame(maxWidth: 460, maxHeight: .infinity)
            .padding(.horizontal, 20)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(Color(.systemGroupedBackground)).tint(brandAccent)
        .sheet(item: $sheet, onDismiss: {
            dashboard.inboxVisible = false
            Task {
                await notifications.refreshPermission()
                if !phone.loggedIn { await phone.connect(password: "") }
            }
        }) { destination in
            switch destination {
            case .connection: ConnectionView(phone: phone)
            case .messages: MessagesSheet(api: phone.api)
            case .notifications: NotificationSettingsView(notifications: notifications)
            case .number: NumberEntryView(number: $phone.number)
            case .error: ErrorDetailView(message: phone.error)
            }
        }
        .task {
            await notifications.refreshPermission()
            await phone.connect(password: "")
        }
        .task(id: pollingKey) {
            guard phone.loggedIn, phase == .active else { return }
            await notifications.refreshPermission()
            while !Task.isCancelled {
                await notifications.attach(api: phone.api, iccid: phone.savedConnection.iccid)
                await dashboard.refresh(api: phone.api, notifications: notifications)
                do { try await Task.sleep(for: .seconds(4)) } catch { return }
            }
        }
        .onChange(of: phase) { _, value in
            if value == .active { Task { await notifications.refreshPermission() } }
        }
        .onChange(of: phone.loggedIn) { _, loggedIn in if !loggedIn { dashboard.reset(); notifications.detach() } }
        .onChange(of: phone.hasCall) { _, active in
            showKeypad = false
            if active { sheet = nil; dashboard.inboxVisible = false }
        }
        .onChange(of: notifications.openInbox) { _, requested in
            if requested { notifications.openInbox = false; openMessages() }
        }
    }

    private func home(compact: Bool) -> some View {
        VStack(spacing: compact ? 8 : 12) {
            HStack(spacing: 10) {
                Image("ModuleMark").resizable().frame(width: 34, height: 34).clipShape(RoundedRectangle(cornerRadius: 9))
                Text("小电波").font(.title2.weight(.bold))
                Spacer()
                Button { sheet = .notifications } label: {
                    Image(systemName: notifications.enabled && notifications.permitted ? "bell.badge" : "bell").frame(width: 44, height: 44)
                }.accessibilityLabel("通知设置").accessibilityIdentifier("通知设置")
                Button { sheet = .connection } label: { Image(systemName: "gearshape").frame(width: 44, height: 44) }.accessibilityLabel("连接设置")
            }
            connectionCard
            if !phone.error.isEmpty {
                Button { sheet = .error } label: {
                    HStack(spacing: 6) {
                        Image(systemName: "exclamationmark.circle")
                        Text(phone.error).lineLimit(1)
                        Spacer(minLength: 0)
                        Image(systemName: "chevron.right")
                    }.font(.caption).foregroundStyle(.red)
                }.accessibilityLabel("查看错误详情：\(phone.error)")
            }
            if phone.hasCall { callPanel(compact: compact).frame(maxHeight: .infinity, alignment: .center) }
            else { dialPanel(compact: compact).frame(maxHeight: .infinity, alignment: .center) }
            messageCard
            Button { sheet = .notifications } label: {
                HStack(spacing: 5) {
                    Image(systemName: "info.circle")
                    Text(notifications.label + " · 通知设置")
                    Spacer(minLength: 0)
                    Image(systemName: "chevron.right")
                }.font(.caption).foregroundStyle(.secondary).frame(minHeight: 32)
            }
        }.padding(.top, 4).padding(.bottom, 4)
    }

    private var connectionCard: some View {
        Button { sheet = .connection } label: {
            HStack(spacing: 10) {
                Image(systemName: "antenna.radiowaves.left.and.right").font(.title3).foregroundStyle(phone.connected ? brandAccent : .orange)
                VStack(alignment: .leading, spacing: 3) {
                    Text(phone.connected ? "模块已连接" : phone.busy ? "正在连接…" : "连接模块").font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
                    Text(connectionSubtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer(minLength: 4)
                Text(phone.connected ? (phone.status?.audio.available == true ? "语音就绪" : "语音待就绪") : "待连接")
                    .font(.caption.weight(.medium)).foregroundStyle(phone.connected ? brandAccent : .secondary)
                    .padding(.horizontal, 9).padding(.vertical, 6).background(brandAccent.opacity(0.07), in: Capsule())
            }.padding(12).background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 16))
        }.buttonStyle(.plain)
    }
    private var connectionSubtitle: String {
        guard phone.connected else { return "接好 USB 后，点此登录或重试" }
        if !dashboard.simState.isEmpty && dashboard.simState != "READY" { return "SIM：\(dashboard.simState)" }
        if let network = dashboard.network {
            return [network.technology, network.registration, network.rsrp].filter { !$0.isEmpty }.joined(separator: " · ")
        }
        return "USB 直连 · 正在读取网络状态"
    }
    private func dialPanel(compact: Bool) -> some View {
        VStack(spacing: compact ? 10 : 16) {
            Button { sheet = .number } label: {
                VStack(spacing: 2) {
                    Text(phone.number.isEmpty ? "输入电话号码" : phone.number)
                        .font(.system(size: phone.number.isEmpty ? 23 : 29, weight: .medium, design: .rounded))
                        .foregroundStyle(phone.number.isEmpty ? .secondary : .primary).minimumScaleFactor(0.55).lineLimit(1)
                    Text("使用模块 SIM 拨打").font(.caption).foregroundStyle(.secondary)
                }.frame(maxWidth: .infinity, minHeight: compact ? 48 : 62)
            }.buttonStyle(.plain).accessibilityIdentifier("dialNumber").accessibilityLabel("编辑电话号码").accessibilityValue(phone.number.isEmpty ? "未输入" : phone.number)
            keypad(inCall: false, compact: compact)
            HStack(spacing: 26) {
                Color.clear.frame(width: 64, height: 1).accessibilityHidden(true)
                Button { Task { await phone.dial() } } label: {
                    Image(systemName: "phone.fill").font(.system(size: 27, weight: .semibold))
                        .frame(width: compact ? 58 : 66, height: compact ? 58 : 66)
                        .foregroundStyle(.white).background(canDial ? brandAccent : Color(.systemGray3), in: Circle())
                }.disabled(!canDial).accessibilityLabel("拨打电话")
                Button { if !phone.number.isEmpty { phone.number.removeLast() } } label: {
                    Image(systemName: "delete.left").font(.title3).frame(width: 64, height: 52)
                }.opacity(phone.number.isEmpty ? 0 : 1).disabled(phone.number.isEmpty)
                    .accessibilityLabel("删除一位").contextMenu { Button("清空号码", role: .destructive) { phone.number = "" } }
            }.frame(maxWidth: .infinity)

        }
    }
    private var canDial: Bool { phone.connected && phone.savedConnection.simState == "READY" && phone.status?.audio.available == true && PhoneNumber.normalize(phone.number) != nil }
    private func callPanel(compact: Bool) -> some View {
        VStack(spacing: compact ? 10 : 16) {
            if compact && showKeypad {
                HStack {
                    Text(phone.current?.stateLabel ?? "电话").font(.caption).foregroundStyle(brandAccent)
                    Text(phone.current?.number ?? phone.number).font(.title3).lineLimit(1).minimumScaleFactor(0.6)
                }.frame(minHeight: 28)
            } else {
            VStack(spacing: 4) {
                Text(phone.current?.stateLabel ?? "正在发起电话").font(.subheadline).foregroundStyle(brandAccent)
                Text(phone.current?.number.isEmpty == false ? phone.current!.number : phone.number)
                    .font(.system(size: 30, weight: .medium, design: .rounded)).minimumScaleFactor(0.6).lineLimit(1)
                Label(phone.microphoneConnected ? "音频已连接" : "正在连接音频", systemImage: "waveform").font(.caption).foregroundStyle(.secondary)
            }.padding(.vertical, 8)
            }
            if phone.current?.isIncoming == true {
                Button { Task { await phone.answer() } } label: {
                    Label("接听", systemImage: "phone.fill").font(.headline).frame(maxWidth: .infinity, minHeight: 52)
                }.buttonStyle(.borderedProminent).tint(brandAccent)
            } else {
                HStack(spacing: 8) {
                    callAction(phone.muted ? "取消静音" : "静音", symbol: phone.muted ? "mic.slash.fill" : "mic.fill", active: phone.muted, compact: compact && showKeypad) { phone.toggleMute() }
                    callAction("键盘", symbol: "circle.grid.3x3.fill", active: showKeypad, compact: compact && showKeypad) { showKeypad.toggle() }
                    callAction("扬声器", symbol: "speaker.wave.2.fill", active: phone.speaker, compact: compact && showKeypad) { phone.toggleSpeaker() }
                }
            }
            if showKeypad { keypad(inCall: true, compact: compact) }
            Button { phone.endCall() } label: {
                Label(phone.current?.isIncoming == true ? "拒接" : "挂断", systemImage: "phone.down.fill")
                    .font(.headline).frame(maxWidth: .infinity, minHeight: 50).foregroundStyle(.white).background(Color.red, in: Capsule())
            }
        }
    }
    private func callAction(_ title: String, symbol: String, active: Bool, compact: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            VStack(spacing: 6) { Image(systemName: symbol).font(.title3); Text(title).font(.caption) }
                .frame(maxWidth: .infinity, minHeight: compact ? 50 : 64)
                .background(active ? brandAccent.opacity(0.15) : Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 14))
        }.foregroundStyle(.primary)
    }
    private func keypad(inCall: Bool, compact: Bool) -> some View {
        VStack(spacing: compact ? 7 : 10) {
            ForEach(keys, id: \.self) { row in
                HStack(spacing: 26) {
                    ForEach(row, id: \.self) { key in
                        Button { if inCall { phone.sendDigit(key) } else if phone.number.count < 24 { phone.number += key } } label: {
                            VStack(spacing: 0) {
                                Text(key).font(.system(size: compact ? 25 : 29, weight: .regular, design: .rounded))
                                Text(letters[key] ?? " ").font(.system(size: 9, weight: .semibold)).tracking(1.4).foregroundStyle(.secondary)
                            }.frame(width: inCall ? (compact ? 44 : 54) : (compact ? 54 : 66), height: inCall ? (compact ? 44 : 54) : (compact ? 54 : 66))
                                .background(Color(.secondarySystemGroupedBackground), in: Circle())
                        }.buttonStyle(.plain).foregroundStyle(.primary).accessibilityLabel(key)
                            .contextMenu { if key == "0", !inCall { Button("添加国际区号 +") { phone.number += "+" } } }
                    }
                }
            }
        }
    }
    private var messageCard: some View {
        Button(action: openMessages) {
            HStack(spacing: 12) {
                Image(systemName: "bubble.left.and.bubble.right.fill").font(.title3).foregroundStyle(brandAccent)
                    .frame(width: 40, height: 40).background(brandAccent.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
                VStack(alignment: .leading, spacing: 5) {
                    HStack {
                        Text("短信与模块").font(.subheadline.weight(.semibold)); Spacer()
                        if dashboard.unreadCount > 0 { Text("\(dashboard.unreadCount) 条新短信").font(.caption.weight(.semibold)).foregroundStyle(brandAccent) }
                        else if dashboard.loaded { Text("\(dashboard.messages.count) 条").font(.caption).foregroundStyle(.secondary) }
                    }
                    Text(messageSummary).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
                Image(systemName: "chevron.right").font(.caption).foregroundStyle(.secondary)
            }.padding(13).background(Color(.secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 17))
        }.buttonStyle(.plain).accessibilityIdentifier("openMessages")
    }
    private var messageSummary: String {
        if !dashboard.error.isEmpty { return dashboard.error }
        if !phone.connected { return "查看短信、SIM 与网络设置" }
        if let latest = dashboard.latest { return "\(latest.sender) · \(latest.body.replacingOccurrences(of: "\n", with: " "))" }
        return dashboard.loaded ? "暂无短信 · 点此查看或发送" : "正在同步短信…"
    }
    private func openMessages() {
        dashboard.inboxVisible = true; sheet = .messages
        Task { await dashboard.markRead(notifications: notifications) }
    }
}
