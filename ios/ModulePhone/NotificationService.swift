import SwiftUI
import UserNotifications
import PushKit

@MainActor final class NotificationService: NSObject, ObservableObject, UNUserNotificationCenterDelegate {
    static let shared = NotificationService()
    @Published var callsEnabled: Bool { didSet {
        defaults.set(callsEnabled, forKey: "backgroundCallsEnabled")
        startPushRegistration()
        Task { await syncPush() }
    } }
    @Published var pushStatus: PushStatus?
    var pushRegistry: PKPushRegistry?
    var pushToken = ""
    var voipToken = ""
    var pushAPI: ModuleAPI?
    var pushICCID = ""
    var syncingPush = false
    var registrationFingerprint = ""
    var lastPushSync = Date.distantPast
    let installationID: String
    @Published private(set) var authorization: UNAuthorizationStatus = .notDetermined
    @Published private(set) var bannersAllowed = false
    @Published var enabled: Bool { didSet {
        defaults.set(enabled, forKey: "smsAlertsEnabled")
        Task { await syncPush() }
        if !enabled {
            center.removeAllPendingNotificationRequests()
            center.removeAllDeliveredNotifications()
            Task { try? await center.setBadgeCount(0) }
        }
    } }
    @Published var error = ""
    @Published var openInbox = false
    private let center: UNUserNotificationCenter
    private let defaults: UserDefaults
    var permitted: Bool { authorization == .authorized || authorization == .provisional || authorization == .ephemeral }
    var label: String {
        if authorization == .denied { return "系统未允许通知" }
        if !enabled || !permitted { return "短信提醒未开启" }
        if backgroundSMSRegistered { return bannersAllowed ? "后台短信推送已登记" : "后台推送已登记 · 无横幅" }
        return "短信提醒已开启 · 后台待登记"
    }
    init(center: UNUserNotificationCenter = .current(), defaults: UserDefaults = .standard) {
        self.center = center; self.defaults = defaults
        enabled = defaults.bool(forKey: "smsAlertsEnabled")
        callsEnabled = defaults.bool(forKey: "backgroundCallsEnabled")
        installationID = defaults.string(forKey: "pushInstallationID") ?? UUID().uuidString
        defaults.set(installationID, forKey: "pushInstallationID")
        super.init()
        center.delegate = self
    }
    func refreshPermission() async {
        let settings = await center.notificationSettings()
        authorization = settings.authorizationStatus
        bannersAllowed = settings.alertSetting == .enabled
    }
    func enable() async {
        error = ""
        do {
            enabled = try await center.requestAuthorization(options: [.alert, .sound, .badge])
            await refreshPermission()
            startPushRegistration()
            await syncPush()
        } catch { self.error = error.localizedDescription }
    }
    func notifyMessages(count: Int, unread: Int) async {
        guard count > 0, enabled, permitted, !backgroundSMSRegistered else { return }
        let content = UNMutableNotificationContent()
        content.title = count == 1 ? "模块收到新短信" : "模块收到 \(count) 条新短信"
        content.body = "点按查看短信。"
        content.sound = .default
        content.badge = NSNumber(value: unread)
        content.threadIdentifier = "module-sms"
        content.userInfo = ["destination": "inbox"]
        do { try await center.add(UNNotificationRequest(identifier: "sms-\(UUID().uuidString)", content: content, trigger: nil)) }
        catch { self.error = "短信已收到，通知未能显示：\(error.localizedDescription)" }
    }
    func testNotification() async {
        guard enabled, permitted else { return }
        error = ""
        let content = UNMutableNotificationContent()
        content.title = "模块电话 · 通知测试"
        content.body = "本机横幅正常。后台接收请使用“测试后台推送”。"
        content.sound = .default
        do { try await center.add(UNNotificationRequest(identifier: "notification-test", content: content, trigger: UNTimeIntervalNotificationTrigger(timeInterval: 3, repeats: false))) }
        catch { self.error = error.localizedDescription }
    }
    func updateBadge(_ count: Int) async {
        if permitted { try? await center.setBadgeCount(enabled ? count : 0) }
    }
    func clearMessages() async {
        let delivered = await center.deliveredNotifications()
        center.removeDeliveredNotifications(withIdentifiers: delivered.filter { $0.request.content.threadIdentifier == "module-sms" }.map { $0.request.identifier })
        await updateBadge(0)
    }
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification, withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        Task { @MainActor in
            completionHandler(enabled ? [.banner, .list, .sound, .badge] : [])
        }
    }
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse, withCompletionHandler completionHandler: @escaping () -> Void) {
        let inbox = response.notification.request.content.userInfo["destination"] as? String == "inbox"
        Task { @MainActor in
            if inbox { openInbox = true }
            completionHandler()
        }
    }
}
