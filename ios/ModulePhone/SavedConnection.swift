import CryptoKit
import Foundation
import ModuleCore
import Security

struct SIMBinding: Decodable {
    let iccid: String
    let sim: State
    let bindingVersion: Int
    struct State: Decodable { let state: String; let blocked: Bool }
    enum CodingKeys: String, CodingKey { case iccid, sim; case bindingVersion = "binding_version" }
}

protocol CredentialStoring {
    func read(endpoint: String) throws -> SavedModuleCredentials?
    func write(_ record: SavedModuleCredentials, endpoint: String) throws
    func remove(endpoint: String) throws
}

struct KeychainCredentialStore: CredentialStoring {
    var service = "com.shield.modulephone.connection.v1"
    private func query(_ endpoint: String) -> [String: Any] {
        let account = SHA256.hash(data: Data(endpoint.utf8)).map { String(format: "%02x", $0) }.joined()
        return [kSecClass as String:kSecClassGenericPassword, kSecAttrService as String:service,
                kSecAttrAccount as String:account, kSecAttrSynchronizable as String:false]
    }
    func read(endpoint: String) throws -> SavedModuleCredentials? {
        var q = query(endpoint); q[kSecReturnData as String] = true; q[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        let code = SecItemCopyMatching(q as CFDictionary, &result)
        if code == errSecItemNotFound { return nil }
        guard code == errSecSuccess, let data = result as? Data else { throw ModuleError.message("系统钥匙串暂不可用，请解锁 iPhone 后重试") }
        return try JSONDecoder().decode(SavedModuleCredentials.self, from: data)
    }
    func write(_ record: SavedModuleCredentials, endpoint: String) throws {
        let data = try JSONEncoder().encode(record)
        let attributes: [String:Any] = [kSecValueData as String:data, kSecAttrAccessible as String:kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
        let code = SecItemUpdate(query(endpoint) as CFDictionary, attributes as CFDictionary)
        if code == errSecItemNotFound {
            var q = query(endpoint); q.merge(attributes) { _, new in new }
            guard SecItemAdd(q as CFDictionary, nil) == errSecSuccess else { throw ModuleError.message("未能保存到系统钥匙串") }
        } else if code != errSecSuccess { throw ModuleError.message("未能更新系统钥匙串") }
    }
    func remove(endpoint: String) throws {
        let code = SecItemDelete(query(endpoint) as CFDictionary)
        guard code == errSecSuccess || code == errSecItemNotFound else { throw ModuleError.message("未能删除保存的凭据") }
    }
}

@MainActor final class SavedConnection: ObservableObject {
    @Published private(set) var cardSuffix = ""
    @Published private(set) var hasPassword = false
    @Published private(set) var hasPIN = false
    @Published private(set) var pinPaused = false
    @Published private(set) var simState = ""
    @Published private(set) var blocked = false
    @Published private(set) var notice = ""
    @Published private(set) var busy = false
    private(set) var iccid = ""
    private var endpoint = ""
    private var record: SavedModuleCredentials?
    private let store: CredentialStoring
    init(store: CredentialStoring = KeychainCredentialStore()) { self.store = store }

    func inspect(api: ModuleAPI) async throws {
        let binding = try JSONDecoder().decode(SIMBinding.self, from: await api.request("api/sim/binding"))
        guard binding.bindingVersion == 1, !binding.iccid.isEmpty else { throw ModuleError.message("模块暂不能确认 SIM 身份，未使用保存的凭据") }
        try apply(binding, endpoint: api.endpoint.url.absoluteString)
    }
    // Shared by readback and unit tests. Swapping back also requires re-entry:
    // only the currently bound card can retain credentials.
    func apply(_ binding: SIMBinding, endpoint: String) throws {
        let saved = try store.read(endpoint: endpoint)
        if let saved, !saved.matches(binding.iccid) {
            var replacement = SavedModuleCredentials(iccid: binding.iccid)
            replacement.requiresManualLogin = true
            try store.write(replacement, endpoint: endpoint)
            notice = "检测到换卡，已清除原卡保存的密码和 PIN，请重新输入。"
            record = replacement
        } else { record = saved }
        self.endpoint = endpoint; iccid = binding.iccid
        cardSuffix = String(binding.iccid.suffix(4)); simState = binding.sim.state; blocked = binding.sim.blocked
        publish()
    }
    var requiresManualLogin: Bool { record?.requiresManualLogin == true }
    var passwordForLogin: String? { record?.matches(iccid) == true && record?.passwordRejected == false ? record?.password : nil }
    func rememberPassword(_ value: String, enabled: Bool) throws {
        guard !iccid.isEmpty else { return }
        var next = record ?? SavedModuleCredentials(iccid: iccid)
        next.password = enabled ? value : nil; next.passwordRejected = false; next.requiresManualLogin = false
        try save(next)
    }
    func rejectSavedPassword() {
        guard var next = record else { return }; next.passwordRejected = true
        try? save(next); notice = "保存的访问密码已失效，请重新输入。"
    }
    func forget() {
        do { try store.remove(endpoint: endpoint); record = nil; publish(); notice = "已删除这台 iPhone 保存的密码和 PIN。" }
        catch { notice = error.localizedDescription }
    }
    func forgetPassword() {
        guard var next = record else { return }
        next.password = nil; next.passwordRejected = false
        do { try save(next) } catch { notice = error.localizedDescription }
    }
    func forgetPIN() {
        guard var next = record else { return }
        next.pin = nil; next.pinAttemptPending = false
        do { try save(next); notice = "已关闭 PIN 自动解锁。" } catch { notice = error.localizedDescription }
    }
    func savePIN(_ pin: String, api: ModuleAPI) async {
        guard !busy else { return }; busy = true; defer { busy = false }
        do {
            guard pin.range(of: #"^[0-9]{4,8}$"#, options: .regularExpression) != nil else { throw ModuleError.message("PIN 必须为 4–8 位数字") }
            let expected = iccid
            try await inspect(api: api)
            guard !expected.isEmpty, expected == iccid else { throw ModuleError.message("SIM 卡已变化，请重新登录后输入该卡的 PIN") }
            guard simState == "READY" || simState == "SIM PIN", !blocked else { throw ModuleError.message("当前不能自动解锁，请先在短信与模块页面处理 SIM 状态") }
            var next = record ?? SavedModuleCredentials(iccid: iccid)
            next.pin = pin; next.pinAttemptPending = false
            try save(next)
            if simState == "SIM PIN" { try await attemptSavedPIN(api: api) }
            else { notice = "PIN 已保存；下次打开 App 时可为同一张卡自动解锁。当前卡已解锁，本次未提交 PIN。" }
        } catch { notice = error.localizedDescription }
    }
    func attemptSavedPIN(api: ModuleAPI) async throws {
        guard var next = record, next.mayUnlock(iccid: iccid, state: simState, blocked: blocked), let pin = next.pin else { return }
        // Persist BEFORE submitting. A timeout, process kill or reboot cannot
        // cause an unconfirmed PIN to be automatically retried.
        next.pinAttemptPending = true; try save(next)
        do {
            let data = try await api.request("api/sim/unlock", body: ["pin":pin, "expected_iccid":iccid, "request_id":UUID().uuidString])
            let result = try JSONDecoder().decode(SIMBinding.State.self, from: data)
            simState = result.state; blocked = result.blocked
            guard result.state == "READY" else { throw ModuleError.message("尚未确认解锁成功，已暂停自动尝试") }
            next.pinAttemptPending = false; try save(next); notice = "SIM 已自动解锁。"
        } catch {
            notice = "PIN 解锁未确认，自动尝试已停止。请在短信与模块页面核对。"
            throw ModuleError.message(notice)
        }
    }
    private func save(_ next: SavedModuleCredentials) throws { try store.write(next, endpoint: endpoint); record = next; publish() }
    private func publish() {
        hasPassword = record?.password != nil; hasPIN = record?.pin != nil
        pinPaused = record?.pinAttemptPending == true || blocked
    }
}
