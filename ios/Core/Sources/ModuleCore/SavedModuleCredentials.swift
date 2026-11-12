import Foundation

public struct SavedModuleCredentials: Codable {
    public var iccid: String
    public var password: String?
    public var pin: String?
    public var pinAttemptPending = false
    public var passwordRejected = false
    public var requiresManualLogin = false
    public init(iccid: String) { self.iccid = iccid }
    public func matches(_ currentICCID: String) -> Bool { !currentICCID.isEmpty && iccid == currentICCID }
    public func mayUnlock(iccid: String, state: String, blocked: Bool) -> Bool {
        matches(iccid) && !requiresManualLogin && pin != nil && !pinAttemptPending && state == "SIM PIN" && !blocked
    }
}
