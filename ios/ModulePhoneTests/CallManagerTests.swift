import Foundation
import XCTest
@testable import ModulePhone

final class CallManagerTests: XCTestCase {
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

private final class CancelledRequest: URLProtocol {
 override class func canInit(with request: URLRequest) -> Bool { true }
 override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
 override func startLoading() { client?.urlProtocol(self, didFailWithError: URLError(.cancelled)) }
 override func stopLoading() {}
}
