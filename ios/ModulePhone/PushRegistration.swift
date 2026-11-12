import CryptoKit
import Foundation
import PushKit
import UIKit
import ModuleCore

struct PushStatus: Decodable {
    let configured: Bool
    let registered: Bool
    let deviceID: String
    let smsEnabled: Bool
    let callsEnabled: Bool
    let queued: Int
    let accepted: Int
    let error: String
    enum CodingKeys: String, CodingKey {
        case configured, registered, queued, accepted, error
        case deviceID = "device_id", smsEnabled = "sms_enabled", callsEnabled = "calls_enabled"
    }
}

struct IncomingPush {
    let callID: String
    let uuid: UUID
    init?(_ payload: [AnyHashable: Any]) {
        guard let id = payload["call_id"] as? String,
              id.range(of: "^[a-f0-9]{24}$", options: .regularExpression) != nil,
              let raw = payload["call_uuid"] as? String, let uuid = UUID(uuidString: raw),
              uuid == Self.uuid(for: id) else { return nil }
        callID = id; self.uuid = uuid
    }
    static func uuid(for id: String) -> UUID {
        var bytes = Array(SHA256.hash(data: Data(("module-call:" + id).utf8)).prefix(16))
        bytes[6] = (bytes[6] & 15) | 0x40; bytes[8] = (bytes[8] & 63) | 0x80
        let s = bytes.map { String(format: "%02x", $0) }.joined()
        let chars = Array(s)
        return UUID(uuidString: [String(chars[0..<8]), String(chars[8..<12]), String(chars[12..<16]), String(chars[16..<20]), String(chars[20..<32])].joined(separator: "-"))!
    }
}

@MainActor final class PushAppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        NotificationService.shared.startPushRegistration()
        return true
    }
    func application(_ application: UIApplication, didRegisterForRemoteNotificationsWithDeviceToken deviceToken: Data) {
        NotificationService.shared.setPushToken(deviceToken, voip: false)
    }
    func application(_ application: UIApplication, didFailToRegisterForRemoteNotificationsWithError error: Error) {
        NotificationService.shared.error = "苹果推送登记失败，请检查 iPhone 网络后重试。"
    }
}

extension NotificationService: @preconcurrency PKPushRegistryDelegate {
    func startPushRegistration() {
        UIApplication.shared.registerForRemoteNotifications()
        if pushRegistry == nil {
            let registry = PKPushRegistry(queue: .main)
            registry.delegate = self
            pushRegistry = registry
        }
        pushRegistry?.desiredPushTypes = callsEnabled ? [.voIP] : []
    }
    func setPushToken(_ value: Data, voip: Bool) {
        let token = value.map { String(format: "%02x", $0) }.joined()
        if voip { voipToken = token } else { pushToken = token }
        registrationFingerprint = ""
        Task { await syncPush() }
    }
    func pushRegistry(_ registry: PKPushRegistry, didUpdate pushCredentials: PKPushCredentials, for type: PKPushType) {
        if type == .voIP { setPushToken(pushCredentials.token, voip: true) }
    }
    func pushRegistry(_ registry: PKPushRegistry, didInvalidatePushTokenFor type: PKPushType) {
        if type == .voIP { voipToken = ""; registrationFingerprint = "" }
    }
    func pushRegistry(_ registry: PKPushRegistry, didReceiveIncomingPushWith payload: PKPushPayload, for type: PKPushType, completion: @escaping () -> Void) {
        // Report synchronously on the PushKit callback's run loop, before network I/O.
        CallManager.shared.receiveIncomingPush(payload.dictionaryPayload, completion: completion)
    }
    func attach(api: ModuleAPI, iccid: String) async {
        if pushAPI?.endpoint.url != api.endpoint.url || pushICCID != iccid {
            registrationFingerprint = ""; pushStatus = nil
        }
        pushAPI = api; pushICCID = iccid
        await syncPush()
    }
    func detach() { pushAPI = nil; pushICCID = ""; registrationFingerprint = ""; pushStatus = nil }
    var backgroundSMSRegistered: Bool {
        pushStatus?.configured == true && pushStatus?.smsEnabled == true && pushStatus?.deviceID == installationID
    }
    func syncPush() async {
        guard !syncingPush, let api = pushAPI, !pushICCID.isEmpty else { return }
        syncingPush = true; defer { syncingPush = false }
        let card = pushICCID
        let sms = enabled && permitted
        let fingerprint = "\(api.endpoint.url)|\(card)|\(sms)|\(callsEnabled)|\(pushToken)|\(voipToken)"
        if registrationFingerprint == fingerprint, Date().timeIntervalSince(lastPushSync) < 30 { return }
        do {
            let data: Data
            if (sms && pushToken.isEmpty) || (callsEnabled && voipToken.isEmpty) {
                data = try await api.request("api/push/status")
            } else {
                data = try await api.request("api/push/register", body: [
                    "expected_iccid":card, "device_id":installationID, "token":pushToken,
                    "voip_token":voipToken, "enabled":sms, "calls_enabled":callsEnabled, "environment":"development"
                ])
                if card == pushICCID { registrationFingerprint = fingerprint }
            }
            guard card == pushICCID, api === pushAPI else { return }
            pushStatus = try JSONDecoder().decode(PushStatus.self, from: data)
            lastPushSync = Date()
            error = pushStatus?.error ?? ""
        } catch {
            guard card == pushICCID, api === pushAPI else { return }
            self.error = "后台通知尚未同步：\(error.localizedDescription)"
            if let data = try? await api.request("api/push/status"), card == pushICCID, api === pushAPI {
                pushStatus = try? JSONDecoder().decode(PushStatus.self, from: data)
            }
        }
    }
    func testBackgroundPush() async {
        await syncPush()
        guard let api = pushAPI, backgroundSMSRegistered else { error = "请先连接模块并开启后台短信通知。"; return }
        do {
            _ = try await api.request("api/push/test", body: [:])
            error = "测试已排队，请现在锁屏；约 8 秒后由模块发送。"
        } catch { self.error = error.localizedDescription }
    }
}
