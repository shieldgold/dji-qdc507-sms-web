import Foundation
import ModuleCore
import WebKit

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
  let (data, response) = try await session.data(for: r)
  guard let response = response as? HTTPURLResponse else { throw ModuleError.message("模块未返回有效响应") }
  guard (200..<300).contains(response.statusCode) else {
   let value = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
   if response.statusCode == 401 { throw ModuleError.message("请先输入模块访问密码登录") }
   throw ModuleError.message(value?["error"] as? String ?? "模块请求失败（\(response.statusCode)）")
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
 func login(password: String) async throws { _ = try await request("api/auth/login", body: ["password": password, "remember": true]) }
 func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) { completionHandler(nil) }
}
