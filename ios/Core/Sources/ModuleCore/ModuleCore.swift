import Foundation

public struct ModuleEndpoint {
 public let url: URL
 public init(_ text: String) throws {
  guard let c = URLComponents(string: text), c.scheme == "http", ["192.168.225.1", "127.0.0.1"].contains(c.host ?? ""), c.user == nil, c.password == nil, c.query == nil, c.fragment == nil, c.path.isEmpty || c.path == "/", let port = c.port, (1...65535).contains(port), let u = c.url else { throw ModuleError.message("请使用模块地址 http://192.168.225.1:8787") }
  url = u
 }
}
public enum ModuleError: LocalizedError {
 case message(String)
 public var errorDescription: String? { if case let .message(s) = self { return s }; return nil }
}
public enum PhoneNumber {
 public static func normalize(_ value: String) -> String? {
  let n = value.replacingOccurrences(of: " ", with: "").replacingOccurrences(of: "-", with: "").replacingOccurrences(of: "(", with: "").replacingOccurrences(of: ")", with: "")
  return n.range(of: #"^\+?[0-9]{3,20}$"#, options: .regularExpression) == nil ? nil : n
 }
}
public struct PCMFramer {
 private var pending = Data()
 public var pendingBytes: Int { pending.count }
 public init() {}
 public mutating func append(_ data: Data) -> [Data] {
  pending.append(data)
  var frames: [Data] = []
  while pending.count >= 320 { frames.append(Data(pending.prefix(320))); pending.removeFirst(320) }
  return frames
 }
}
public struct VoiceCall: Decodable, Identifiable, Equatable {
 public let id: String
 public let index: Int
 public let number: String
 public let direction: String
 public let state: String
 public let requestID: String?
 enum CodingKeys: String, CodingKey { case id, index, number, direction, state; case requestID = "request_id" }
 public var isIncoming: Bool { state == "incoming" || state == "waiting" }
 public var stateLabel: String { ["active":"通话中", "held":"已保持", "dialing":"正在拨号", "alerting":"对方响铃", "incoming":"来电", "waiting":"来电等待"][state] ?? "状态待确认" }
}
public struct CallSnapshot: Decodable {
 public let calls: [VoiceCall]
 public let pending: Bool
 public let updatedAt: String
 public let error: String?
 enum CodingKeys: String, CodingKey { case calls, pending, error; case updatedAt = "updated_at" }
}
public struct AudioStatus: Decodable {
 public let available: Bool
 public let state: String
 public let error: String?
 public let upBytes: UInt64
 public let downBytes: UInt64
 enum CodingKeys: String, CodingKey { case available, state, error; case upBytes = "up_bytes"; case downBytes = "down_bytes" }
}
public struct ModuleCallState: Decodable { public let telephone: CallSnapshot; public let audio: AudioStatus }
public struct AudioTicket: Decodable { public let ticket: String; public let port: UInt16 }
