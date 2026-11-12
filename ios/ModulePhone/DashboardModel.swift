import CryptoKit
import SwiftUI
import ModuleCore

private struct ModuleIdentity: Decodable {
    let authenticated: Bool
    let iccid: String
    let imsi: String
    let sim: SIMState
    struct SIMState: Decodable { let state: String }
}
struct NetworkOverview: Decodable {
    let network: Network
    struct Network: Decodable {
        let technology: String
        let registered: Bool
        let registration: String
        let rsrp: String
    }
}

@MainActor final class DashboardModel: ObservableObject {
    @Published private(set) var messages: [ModuleMessage] = []
    @Published private(set) var unreadCount = 0
    @Published private(set) var network: NetworkOverview.Network?
    @Published private(set) var simState = ""
    @Published private(set) var error = ""
    @Published private(set) var loaded = false
    var inboxVisible = false
    private var tracker = MessageTracker()
    private var scope = ""
    private let defaults: UserDefaults
    init(defaults: UserDefaults = .standard) { self.defaults = defaults }
    var latest: ModuleMessage? { messages.first(where: \.isIncoming) }

    func refresh(api: ModuleAPI, notifications: NotificationService) async {
        do {
            let identity = try JSONDecoder().decode(ModuleIdentity.self, from: await api.request("api/auth/status"))
            try Task.checkCancellation()
            guard identity.authenticated else { reset(); error = "连接模块后查看短信"; return }
            let identityKey = identity.iccid.isEmpty ? identity.imsi : identity.iccid
            guard !identityKey.isEmpty else { reset(); error = "等待 SIM 信息"; return }
            let digest = SHA256.hash(data: Data((api.endpoint.url.absoluteString + identityKey).utf8)).map { String(format: "%02x", $0) }.joined()
            let key = "sms-tracker-" + digest
            if scope != key {
                reset(); scope = key
                tracker = defaults.data(forKey: key).flatMap { try? JSONDecoder().decode(MessageTracker.self, from: $0) } ?? MessageTracker()
            }
            simState = identity.sim.state
            guard simState == "READY" else { error = "SIM 需要处理，请打开短信与模块"; return }
            let snapshot = try JSONDecoder().decode(InboxSnapshot.self, from: await api.request("api/state"))
            try Task.checkCancellation()
            messages = snapshot.messages; loaded = true; error = snapshot.lastError ?? ""
            let added = tracker.ingest(messages)
            if inboxVisible { tracker.markRead() }
            persist()
            if !inboxVisible { await notifications.notifyMessages(count: added.count, unread: unreadCount) }
            await notifications.updateBadge(unreadCount)
            // Overview failures must not prevent a successfully received SMS alert.
            if let data = try? await api.request("api/overview"), !Task.isCancelled {
                network = try? JSONDecoder().decode(NetworkOverview.self, from: data).network
            }
        } catch {
            guard !Task.isCancelled, (error as? URLError)?.code != .cancelled else { return }
            self.error = "短信暂未同步，点按重试连接"
        }
    }
    func markRead(notifications: NotificationService) async {
        tracker.markRead(); persist(); await notifications.clearMessages()
    }
    private func persist() {
        unreadCount = tracker.unread.count
        if !scope.isEmpty, let data = try? JSONEncoder().encode(tracker) { defaults.set(data, forKey: scope) }
    }
    func reset() {
        scope = ""; tracker = MessageTracker(); messages = []; unreadCount = 0
        network = nil; simState = ""; error = ""; loaded = false
    }
}
