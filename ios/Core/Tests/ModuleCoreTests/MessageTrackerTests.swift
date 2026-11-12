import XCTest
@testable import ModuleCore

final class MessageTrackerTests: XCTestCase {
    private func message(_ key: String, direction: String = "in") throws -> ModuleMessage {
        let data = try JSONSerialization.data(withJSONObject: ["key":key, "sender":"test", "body":"text", "time":"2026-11-13", "direction":direction])
        return try JSONDecoder().decode(ModuleMessage.self, from: data)
    }
    func testInitialArchiveDoesNotNotify() throws {
        var tracker = MessageTracker()
        XCTAssertTrue(tracker.ingest([try message("old")]).isEmpty)
        XCTAssertTrue(tracker.initialized)
        XCTAssertTrue(tracker.unread.isEmpty)
    }
    func testNewIncomingNotifiesOnceAndOutgoingNeverDoes() throws {
        var tracker = MessageTracker()
        _ = tracker.ingest([try message("old")])
        let rows = [try message("new"), try message("sent", direction: "out"), try message("old")]
        XCTAssertEqual(tracker.ingest(rows).map(\.key), ["new"])
        XCTAssertTrue(tracker.ingest(rows).isEmpty)
        XCTAssertEqual(tracker.unread.count, 1)
    }
    func testRestartAndReconnectDoNotRepeatAlerts() throws {
        var tracker = MessageTracker()
        _ = tracker.ingest([])
        _ = tracker.ingest([try message("first")])
        tracker = try JSONDecoder().decode(MessageTracker.self, from: JSONEncoder().encode(tracker))
        XCTAssertEqual(tracker.ingest([try message("second"), try message("first")]).map(\.key), ["second"])
        XCTAssertEqual(tracker.unread.count, 2)
    }
    func testReadAndDeletionUpdateNewCountWithoutRenotifying() throws {
        var tracker = MessageTracker()
        _ = tracker.ingest([])
        _ = tracker.ingest([try message("one"), try message("two")])
        XCTAssertTrue(tracker.ingest([try message("one")]).isEmpty)
        XCTAssertEqual(tracker.unread.count, 1)
        tracker.markRead()
        XCTAssertTrue(tracker.ingest([try message("one"), try message("two")]).isEmpty)
        XCTAssertTrue(tracker.unread.isEmpty)
    }
    func testDuplicateIDsInSnapshotOnlyNotifyOnce() throws {
        var tracker = MessageTracker()
        _ = tracker.ingest([])
        XCTAssertEqual(tracker.ingest([try message("one"), try message("one")]).count, 1)
    }
    func testLegacyIdentifiersAreHashedWithoutRepeatingAlerts() throws {
        let data = Data(#"{"initialized":true,"seen":["old"],"unread":["old"]}"#.utf8)
        var tracker = try JSONDecoder().decode(MessageTracker.self, from: data)
        XCTAssertTrue(tracker.ingest([try message("old")]).isEmpty)
        XCTAssertEqual(tracker.unread.count, 1)
        XCTAssertFalse(String(decoding: try JSONEncoder().encode(tracker), as: UTF8.self).contains("old"))
    }
    func testNotificationStateContainsNoMessageContent() throws {
        var tracker = MessageTracker()
        _ = tracker.ingest([try message("10086|2026-11-13|736563726574")])
        let stored = String(decoding: try JSONEncoder().encode(tracker), as: UTF8.self)
        XCTAssertFalse(stored.contains("sender"))
        XCTAssertFalse(stored.contains("body"))
        XCTAssertFalse(stored.contains("10086"))
        XCTAssertFalse(stored.contains("736563726574"))
    }
}
