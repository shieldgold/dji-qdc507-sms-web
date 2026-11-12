import Foundation
import XCTest
import ModuleCore
@testable import ModulePhone

final class CallManagerTests: XCTestCase {
 @MainActor func testSafariLoginURLKeepsOnlyAShortLivedTicketInFragment() async throws {
  let api = ModuleAPI(endpoint: try ModuleEndpoint("http://127.0.0.1:8787"))
  let config = URLSessionConfiguration.ephemeral
  config.protocolClasses = [BrowserTicketRequest.self]
  api.session = URLSession(configuration: config)
  defer { api.session.invalidateAndCancel() }
  let url = try await api.browserLoginURL()
  XCTAssertEqual(url.host, "127.0.0.1")
  XCTAssertEqual(url.port, 8787)
  XCTAssertNil(url.query)
  XCTAssertEqual(url.fragment, "browser-login=" + String(repeating: "a", count: 64))
 }
 @MainActor func testCancelledRefreshPreservesConnectionAndCallError() async {
  let manager = CallManager()
  let config = URLSessionConfiguration.ephemeral
  config.protocolClasses = [CancelledRequest.self]
  manager.api.session = URLSession(configuration: config)
  manager.loggedIn = true
  manager.connected = true
  manager.error = "音频中断：模块音频连接已结束"
  await manager.refresh()
  XCTAssertTrue(manager.connected)
  XCTAssertEqual(manager.error, "音频中断：模块音频连接已结束")
  manager.api.session.invalidateAndCancel()
 }
}

private final class BrowserTicketRequest: URLProtocol {
 override class func canInit(with request: URLRequest) -> Bool { true }
 override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
 override func startLoading() {
  XCTAssertEqual(request.url?.path, "/api/auth/browser/ticket")
  XCTAssertEqual(request.httpMethod, "POST")
  let data = Data(("{\"ticket\":\"" + String(repeating: "a", count: 64) + "\",\"expires_in\":30}").utf8)
  client?.urlProtocol(self, didReceive: HTTPURLResponse(url: request.url!, statusCode: 200, httpVersion: nil, headerFields: ["Content-Type":"application/json"])!, cacheStoragePolicy: .notAllowed)
  client?.urlProtocol(self, didLoad: data); client?.urlProtocolDidFinishLoading(self)
 }
 override func stopLoading() {}
}

private final class CancelledRequest: URLProtocol {
 override class func canInit(with request: URLRequest) -> Bool { true }
 override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
 override func startLoading() { client?.urlProtocol(self, didFailWithError: URLError(.cancelled)) }
 override func stopLoading() {}
}
