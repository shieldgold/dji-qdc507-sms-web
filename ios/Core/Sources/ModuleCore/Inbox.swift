import Foundation
import CryptoKit

public struct ModuleMessage: Decodable, Identifiable, Equatable {
    public let key: String
    public let sender: String
    public let body: String
    public let time: String
    public let direction: String
    public var id: String { key }
    public var isIncoming: Bool { direction == "in" }
}

public struct InboxSnapshot: Decodable {
    public let messages: [ModuleMessage]
    public let lastError: String?
}

/// Only hashed identifiers are persisted (the module key embeds message text). The first successful snapshot establishes a
/// baseline; existing history must never produce a burst of new-message alerts.
public struct MessageTracker: Codable {
    public private(set) var initialized = false
    private var seen: [String] = []
    public private(set) var unread: Set<String> = []
    public init() {}
    private enum CodingKeys: String, CodingKey { case version, initialized, seen, unread }
    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        initialized = try values.decode(Bool.self, forKey: .initialized)
        let oldSeen = try values.decode([String].self, forKey: .seen)
        let oldUnread = try values.decode(Set<String>.self, forKey: .unread)
        if try values.decodeIfPresent(Int.self, forKey: .version) == 2 {
            seen = oldSeen; unread = oldUnread
        } else {
            seen = oldSeen.map(Self.fingerprint); unread = Set(oldUnread.map(Self.fingerprint))
        }
    }
    public func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(2, forKey: .version)
        try values.encode(initialized, forKey: .initialized)
        try values.encode(seen, forKey: .seen)
        try values.encode(unread, forKey: .unread)
    }

    public mutating func ingest(_ messages: [ModuleMessage]) -> [ModuleMessage] {
        let incoming = messages.filter(\.isIncoming)
        let current = Set(incoming.map { Self.fingerprint($0.key) })
        var known = Set(seen)
        var added: [ModuleMessage] = []
        for message in incoming.reversed() {
            let key = Self.fingerprint(message.key)
            guard known.insert(key).inserted else { continue }
            seen.append(key)
            if initialized { added.append(message); unread.insert(key) }
        }
        initialized = true
        unread.formIntersection(current)
        if seen.count > 2000 { seen = Array(seen.suffix(2000)) }
        return added
    }

    private static func fingerprint(_ key: String) -> String {
        SHA256.hash(data: Data(key.utf8)).map { String(format: "%02x", $0) }.joined()
    }

    public mutating func markRead() { unread.removeAll() }
}
