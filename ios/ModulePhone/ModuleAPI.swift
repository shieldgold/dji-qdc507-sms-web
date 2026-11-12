import Foundation
import ModuleCore
import WebKit

struct ModuleHTTPError: LocalizedError {
 let status: Int
 let message: String
 var errorDescription: String? { message }
}

final class ModuleAPI: NSObject, URLSessionTaskDelegate {
 let endpoint: ModuleEndpoint
 lazy var session: URLSession = {
  let c = URLSessionConfiguration.default
  c.timeoutIntervalForRequest = 15
  c.httpCookieStorage = .shared
  c.httpShouldSetCookies = true
  c.requestCachePolicy = .reloadIgnoringLocalCacheData
  return URLSession(configuration: c, delegate: self, delegateQueue: nil)
 }()
 init(endpoint: ModuleEndpoint) { self.endpoint = endpoint }
 func request(_ path: String, body: [String: Any]? = nil) async throws -> Data {
  let url = endpoint.url.appendingPathComponent(path)
  var r = URLRequest(url: url)
  if let body {
   r.httpMethod = "POST"; r.setValue("application/json", forHTTPHeaderField: "Content-Type")
   r.httpBody = try JSONSerialization.data(withJSONObject: body)
  }
  let data: Data
  let response: URLResponse
  do { (data, response) = try await session.data(for: r) }
  catch let failure as URLError where failure.code != .cancelled {
   throw ModuleError.message("暂时连接不到模块，请检查 USB 连接并等待模块启动后重试")
  }
  guard let response = response as? HTTPURLResponse else { throw ModuleError.message("模块未返回有效响应") }
  guard (200..<300).contains(response.statusCode) else {
   let value = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
   throw ModuleHTTPError(status: response.statusCode, message: value?["error"] as? String ?? "模块请求失败（\(response.statusCode)）")
  }
  return data
 }
 func state() async throws -> ModuleCallState { try JSONDecoder().decode(ModuleCallState.self, from: await request("api/calls")) }
 func ticket() async throws -> AudioTicket { try JSONDecoder().decode(AudioTicket.self, from: await request("api/calls/audio", body: [:])) }
 @MainActor func importWebSession() async {
  let cookies = await WKWebsiteDataStore.default().httpCookieStore.allCookies()
  for cookie in cookies where cookie.domain == endpoint.url.host {
   HTTPCookieStorage.shared.setCookie(cookie)
  }
 }
 @MainActor func shareSessionWithWebView() async {
  for cookie in HTTPCookieStorage.shared.cookies(for: endpoint.url) ?? [] {
   await WKWebsiteDataStore.default().httpCookieStore.setCookie(cookie)
  }
 }
 @MainActor func browserLoginURL() async throws -> URL {
  await importWebSession()
  let data = try await request("api/auth/browser/ticket", body: [:])
  struct Ticket: Decodable { let ticket: String }
  let ticket = try JSONDecoder().decode(Ticket.self, from: data).ticket
  guard ticket.count == 64, ticket.allSatisfy({ "0123456789abcdef".contains($0) }),
        var url = URLComponents(url: endpoint.url, resolvingAgainstBaseURL: false) else {
   throw ModuleError.message("模块未返回有效免密链接，请重试")
  }
  url.fragment = "browser-login=" + ticket
  guard let target = url.url else { throw ModuleError.message("无法打开模块网页") }
  return target
 }
 func login(password: String, iccid: String = "") async throws { _ = try await request("api/auth/login", body: ["password": password, "remember": true, "expected_iccid": iccid]) }
 func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) { completionHandler(nil) }
}
