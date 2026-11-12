import XCTest
import ModuleCore
@testable import ModulePhone

final class SavedConnectionTests: XCTestCase {
    private func binding(_ card: String, state: String = "READY", blocked: Bool = false) throws -> SIMBinding {
        let data = try JSONSerialization.data(withJSONObject: ["iccid":card, "sim":["state":state,"blocked":blocked], "binding_version":1])
        return try JSONDecoder().decode(SIMBinding.self, from: data)
    }
    @MainActor func testSwapClearsBothSecretsAndRequiresEntryEvenWhenSwappingBack() throws {
        let store = MemoryCredentials()
        var original = SavedModuleCredentials(iccid: "card-a")
        original.password = "test-password"; original.pin = "1234"
        store.value = original
        let saved = SavedConnection(store: store)
        try saved.apply(binding("card-a"), endpoint: "test")
        XCTAssertNotNil(saved.passwordForLogin)
        XCTAssertTrue(saved.hasPIN)
        try saved.apply(binding("card-b"), endpoint: "test")
        XCTAssertNil(saved.passwordForLogin)
        XCTAssertFalse(saved.hasPIN)
        XCTAssertTrue(saved.requiresManualLogin)
        XCTAssertNil(store.value?.password)
        XCTAssertNil(store.value?.pin)
        let relaunched = SavedConnection(store: store)
        try relaunched.apply(binding("card-b"), endpoint: "test")
        XCTAssertTrue(relaunched.requiresManualLogin)
        try relaunched.apply(binding("card-a"), endpoint: "test")
        XCTAssertTrue(relaunched.requiresManualLogin)
        XCTAssertNil(relaunched.passwordForLogin)
    }
    @MainActor func testRejectedPasswordIsNotAutomaticallyUsedAgain() throws {
        let store = MemoryCredentials()
        let saved = SavedConnection(store: store)
        try saved.apply(binding("card-a"), endpoint: "test")
        try saved.rememberPassword("test-password", enabled: true)
        saved.rejectSavedPassword()
        XCTAssertNil(saved.passwordForLogin)
        try saved.rememberPassword("replacement", enabled: true)
        XCTAssertNotNil(saved.passwordForLogin)
        saved.forgetPassword()
        XCTAssertFalse(saved.hasPassword)
    }
    @MainActor func testFailedPINIsNotRetriedAfterRelaunch() async throws {
        let store = MemoryCredentials()
        var row = SavedModuleCredentials(iccid: "card-a"); row.pin = "1234"; store.value = row
        let api = ModuleAPI(endpoint: try ModuleEndpoint("http://127.0.0.1:8787"))
        let config = URLSessionConfiguration.ephemeral; config.protocolClasses = [FailingPINRequest.self]
        api.session = URLSession(configuration: config)
        defer { api.session.invalidateAndCancel() }
        FailingPINRequest.reset()
        let saved = SavedConnection(store: store)
        try saved.apply(binding("card-a", state: "SIM PIN"), endpoint: "test")
        do { try await saved.attemptSavedPIN(api: api); XCTFail("Expected uncertain result") } catch {}
        XCTAssertTrue(store.value!.pinAttemptPending)
        let restarted = SavedConnection(store: store)
        try restarted.apply(binding("card-a", state: "SIM PIN"), endpoint: "test")
        try await restarted.attemptSavedPIN(api: api)
        XCTAssertEqual(FailingPINRequest.attempts, 1)
    }
    func testKeychainRoundTripUsesAnIsolatedTestItem() throws {
        let store = KeychainCredentialStore(service: "com.shield.modulephone.test.\(UUID())")
        defer { try? store.remove(endpoint: "test") }
        var row = SavedModuleCredentials(iccid: "card-a")
        row.password = "synthetic-test-only"; row.pin = "1234"
        try store.write(row, endpoint: "test")
        let restored = try store.read(endpoint: "test")
        XCTAssertEqual(restored?.iccid, row.iccid)
        XCTAssertEqual(restored?.password, row.password)
        row.pinAttemptPending = true
        try store.write(row, endpoint: "test")
        XCTAssertTrue(try store.read(endpoint: "test")!.pinAttemptPending)
        try store.remove(endpoint: "test")
        XCTAssertNil(try store.read(endpoint: "test"))
    }
}

private final class MemoryCredentials: CredentialStoring {
    var value: SavedModuleCredentials?
    func read(endpoint: String) throws -> SavedModuleCredentials? { value }
    func write(_ record: SavedModuleCredentials, endpoint: String) throws { value = record }
    func remove(endpoint: String) throws { value = nil }
}

private final class FailingPINRequest: URLProtocol {
    private static let lock = NSLock()
    private static var count = 0
    static var attempts: Int { lock.lock(); defer { lock.unlock() }; return count }
    static func reset() { lock.lock(); count = 0; lock.unlock() }
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        Self.lock.lock(); Self.count += 1; Self.lock.unlock()
        client?.urlProtocol(self, didFailWithError: URLError(.timedOut))
    }
    override func stopLoading() {}
}
