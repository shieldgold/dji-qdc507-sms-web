import XCTest
import ModuleCore
@testable import ModulePhone

final class DashboardModelTests: XCTestCase {
    @MainActor func testNewMessagesPersistAndAuthLossClearsThePreviousSIM() async throws {
        let name = "dashboard-test-\(UUID())"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        let api = ModuleAPI(endpoint: try ModuleEndpoint("http://127.0.0.1:8787"))
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [InboxFixtureProtocol.self]
        api.session = URLSession(configuration: config)
        defer { api.session.invalidateAndCancel() }
        let notifications = NotificationService(defaults: defaults)
        let model = DashboardModel(defaults: defaults)
        InboxFixtureProtocol.set(messages: ["old"], authenticated: true)
        await model.refresh(api: api, notifications: notifications)
        XCTAssertTrue(model.loaded)
        XCTAssertEqual(model.unreadCount, 0)
        InboxFixtureProtocol.set(messages: ["new", "old"], authenticated: true)
        await model.refresh(api: api, notifications: notifications)
        XCTAssertEqual(model.unreadCount, 1)
        let reopened = DashboardModel(defaults: defaults)
        await reopened.refresh(api: api, notifications: notifications)
        XCTAssertEqual(reopened.unreadCount, 1)
        await reopened.markRead(notifications: notifications)
        XCTAssertEqual(reopened.unreadCount, 0)
        InboxFixtureProtocol.set(messages: [], authenticated: false)
        await reopened.refresh(api: api, notifications: notifications)
        XCTAssertFalse(reopened.loaded)
        XCTAssertTrue(reopened.messages.isEmpty)
        XCTAssertNil(reopened.network)
    }
    @MainActor func testOverviewFailureDoesNotLoseMessagesAndDifferentSIMGetsItsOwnBaseline() async throws {
        let name = "dashboard-test-\(UUID())"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        let api = ModuleAPI(endpoint: try ModuleEndpoint("http://127.0.0.1:8787"))
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [InboxFixtureProtocol.self]
        api.session = URLSession(configuration: config)
        defer { api.session.invalidateAndCancel() }
        let notifications = NotificationService(defaults: defaults)
        let model = DashboardModel(defaults: defaults)
        InboxFixtureProtocol.set(messages: [], authenticated: true)
        await model.refresh(api: api, notifications: notifications)
        InboxFixtureProtocol.set(messages: ["new"], authenticated: true, overviewFails: true)
        await model.refresh(api: api, notifications: notifications)
        XCTAssertEqual(model.messages.count, 1)
        XCTAssertEqual(model.unreadCount, 1)
        InboxFixtureProtocol.set(messages: ["other-history"], authenticated: true, identity: "SIM-B")
        await model.refresh(api: api, notifications: notifications)
        XCTAssertEqual(model.unreadCount, 0)
        XCTAssertEqual(model.latest?.key, "other-history")
    }
}

private final class InboxFixtureProtocol: URLProtocol {
    private static let lock = NSLock()
    private static var fixtures: [String: (Int, Data)] = [:]
    static func set(messages: [String], authenticated: Bool, identity: String = "SIM-A", overviewFails: Bool = false) {
        let rows = messages.map { ["key":$0, "sender":"test", "body":"sample", "direction":"in", "time":"2026-11-13"] }
        let payloads: [String: (Int, Any)] = [
            "/api/auth/status": (200, ["authenticated":authenticated, "iccid":identity, "imsi":"", "sim":["state":"READY"]]),
            "/api/state": (200, ["messages":rows]),
            "/api/overview": (overviewFails ? 503 : 200, ["network":["technology":"LTE", "registered":true, "registration":"已注册", "rsrp":"-80 dBm"]])
        ]
        lock.lock(); defer { lock.unlock() }
        fixtures = payloads.mapValues { ($0.0, try! JSONSerialization.data(withJSONObject: $0.1)) }
    }
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        Self.lock.lock(); let row = Self.fixtures[request.url!.path]; Self.lock.unlock()
        guard let (code, data) = row else { client?.urlProtocol(self, didFailWithError: URLError(.badServerResponse)); return }
        client?.urlProtocol(self, didReceive: HTTPURLResponse(url: request.url!, statusCode: code, httpVersion: "HTTP/1.1", headerFields: ["Content-Type":"application/json"])!, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: data)
        client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}
