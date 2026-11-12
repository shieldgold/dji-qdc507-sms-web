import XCTest
import ModuleCore
@testable import ModulePhone

final class PushRegistrationTests: XCTestCase {
    func testCallPushRejectsMalformedOrMismatchedIdentifiers() {
        let id = "abcd1234abcd1234abcd1234"
        let uuid = IncomingPush.uuid(for: id)
        XCTAssertEqual(uuid.uuidString.lowercased(), "0162a0a6-3d46-4427-95df-6cea76f87699")
        XCTAssertNotNil(IncomingPush(["call_id":id, "call_uuid":uuid.uuidString]))
        XCTAssertNil(IncomingPush(["call_id":id, "call_uuid":UUID().uuidString]))
        XCTAssertNil(IncomingPush(["call_id":"other-call", "call_uuid":uuid.uuidString]))
        XCTAssertNil(IncomingPush(["call_id":id]))
        XCTAssertEqual(IncomingPush.uuid(for: id), uuid)
    }
    @MainActor func testCallsOnlyRegistrationRotatesTokenAndDetaches() async throws {
        let name = "push-tests-\(UUID())"
        let defaults = UserDefaults(suiteName:name)!
        defer { defaults.removePersistentDomain(forName:name) }
        defaults.set(true,forKey:"backgroundCallsEnabled")
        let service = NotificationService(defaults:defaults)
        service.voipToken = String(repeating:"a",count:64)
        let api = ModuleAPI(endpoint:try ModuleEndpoint("http://127.0.0.1:8787"))
        let config = URLSessionConfiguration.ephemeral; config.protocolClasses = [PushFixtureProtocol.self]
        api.session = URLSession(configuration:config)
        defer {api.session.invalidateAndCancel()}
        PushFixtureProtocol.reset()
        await service.attach(api:api,iccid:"test-card")
        XCTAssertEqual(service.pushStatus?.deviceID,service.installationID)
        XCTAssertEqual(service.pushStatus?.callsEnabled,true)
        XCTAssertEqual(PushFixtureProtocol.requests.count,1)
        await service.syncPush()
        XCTAssertEqual(PushFixtureProtocol.requests.count,1)
        service.voipToken = String(repeating:"b",count:64)
        await service.syncPush()
        XCTAssertEqual(PushFixtureProtocol.requests.last?["voip_token"] as? String,service.voipToken)
        XCTAssertEqual(PushFixtureProtocol.requests.last?["expected_iccid"] as? String,"test-card")
        service.detach()
        XCTAssertNil(service.pushStatus)
        XCTAssertFalse(service.backgroundSMSRegistered)
    }
    @MainActor func testDisabledRegistrationContainsNoEnabledPushAndForeignPhoneDoesNotSuppressLocalSMS() async throws {
        let name = "push-tests-\(UUID())"
        let defaults = UserDefaults(suiteName:name)!
        defer { defaults.removePersistentDomain(forName:name) }
        let service = NotificationService(defaults:defaults)
        let api = ModuleAPI(endpoint:try ModuleEndpoint("http://127.0.0.1:8787"))
        let config = URLSessionConfiguration.ephemeral; config.protocolClasses = [PushFixtureProtocol.self]
        api.session = URLSession(configuration:config)
        defer {api.session.invalidateAndCancel()}
        PushFixtureProtocol.reset()
        await service.attach(api:api,iccid:"test-card")
        XCTAssertEqual(PushFixtureProtocol.requests.last?["enabled"] as? Bool,false)
        XCTAssertEqual(PushFixtureProtocol.requests.last?["calls_enabled"] as? Bool,false)
        service.pushStatus = try JSONDecoder().decode(PushStatus.self,from:Data("{\"configured\":true,\"registered\":true,\"device_id\":\"other-phone\",\"sms_enabled\":true,\"calls_enabled\":true,\"queued\":0,\"accepted\":0,\"error\":\"\"}".utf8))
        XCTAssertFalse(service.backgroundSMSRegistered)
    }
}

private final class PushFixtureProtocol: URLProtocol {
    private static let lock = NSLock()
    private static var recorded: [[String:Any]] = []
    static var requests: [[String:Any]] {lock.lock();defer{lock.unlock()};return recorded}
    static func reset() {lock.lock();recorded=[];lock.unlock()}
    override class func canInit(with request: URLRequest)->Bool {true}
    override class func canonicalRequest(for request: URLRequest)->URLRequest {request}
    override func startLoading() {
        let data: Data
        if let body=request.httpBody {data=body}
        else if let stream=request.httpBodyStream {
            stream.open();defer{stream.close()};var body=Data();var bytes=[UInt8](repeating:0,count:4096)
            while stream.hasBytesAvailable {let n=stream.read(&bytes,maxLength:bytes.count);if n<=0 {break};body.append(contentsOf:bytes.prefix(n))};data=body
        } else {data=Data()}
        let value=(try? JSONSerialization.jsonObject(with:data)) as? [String:Any] ?? [:]
        Self.lock.lock();Self.recorded.append(value);Self.lock.unlock()
        let result:[String:Any] = ["configured":true,"registered":true,"device_id":value["device_id"] ?? "", "sms_enabled":value["enabled"] ?? false,"calls_enabled":value["calls_enabled"] ?? false,"queued":0,"accepted":0,"error":""]
        client?.urlProtocol(self,didReceive:HTTPURLResponse(url:request.url!,statusCode:200,httpVersion:nil,headerFields:["Content-Type":"application/json"])!,cacheStoragePolicy:.notAllowed)
        client?.urlProtocol(self,didLoad:try! JSONSerialization.data(withJSONObject:result));client?.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}
