import XCTest
@testable import ModuleCore

final class SavedModuleCredentialsTests: XCTestCase {
    func testPINOnlyAllowedForConfirmedSameCardAndExactLockState() throws {
        var saved = SavedModuleCredentials(iccid: "card-a")
        saved.pin = "1234"
        XCTAssertTrue(saved.mayUnlock(iccid: "card-a", state: "SIM PIN", blocked: false))
        for card in ["", "card-b"] { XCTAssertFalse(saved.mayUnlock(iccid: card, state: "SIM PIN", blocked: false)) }
        for state in ["READY", "UNKNOWN", "SIM PUK", "SIM PIN2", "SIM PUK2"] {
            XCTAssertFalse(saved.mayUnlock(iccid: "card-a", state: state, blocked: false))
        }
        XCTAssertFalse(saved.mayUnlock(iccid: "card-a", state: "SIM PIN", blocked: true))
        saved.pinAttemptPending = true
        let reloaded = try JSONDecoder().decode(SavedModuleCredentials.self, from: JSONEncoder().encode(saved))
        XCTAssertFalse(reloaded.mayUnlock(iccid: "card-a", state: "SIM PIN", blocked: false))
        saved.pinAttemptPending = false; saved.requiresManualLogin = true
        XCTAssertFalse(saved.mayUnlock(iccid: "card-a", state: "SIM PIN", blocked: false))
    }
}
