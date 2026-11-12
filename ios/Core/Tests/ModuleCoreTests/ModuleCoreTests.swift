import XCTest
@testable import ModuleCore
final class ModuleCoreTests: XCTestCase {
 func testOnlyModuleOrLoopbackCanReceiveCredentials() throws {
  XCTAssertEqual(try ModuleEndpoint("http://192.168.225.1:8787/").url.host, "192.168.225.1")
  for bad in ["https://evil.example", "http://192.168.225.1.evil.example:8787", "http://user:pass@192.168.225.1:8787", "http://192.168.225.1:8787/path"] {
   XCTAssertThrowsError(try ModuleEndpoint(bad))
  }
 }
 func testFragmentedPCMStaysSampleAndFrameAligned() {
  var f = PCMFramer()
  XCTAssertTrue(f.append(Data(repeating: 1, count: 319)).isEmpty)
  let frames = f.append(Data([2,3,4]))
  XCTAssertEqual(frames.count, 1)
  XCTAssertEqual(frames[0].count, 320)
  XCTAssertEqual(frames[0].last, 2)
  XCTAssertEqual(f.pendingBytes, 2)
 }
 func testNormalizeDoesNotAcceptATOrServiceCodes() {
  XCTAssertEqual(PhoneNumber.normalize("+86 138-0000-0000"), "+8613800000000")
  XCTAssertNil(PhoneNumber.normalize("10086;ATH"))
  XCTAssertNil(PhoneNumber.normalize("*123#"))
  XCTAssertNil(PhoneNumber.normalize("12"))
 }
 func testCallStateLabelsDoNotClaimAudioAcceptance() throws {
  let data = Data(#"{"id":"a","index":3,"number":"10086","direction":"out","state":"active"}"#.utf8)
  let call = try JSONDecoder().decode(VoiceCall.self, from: data)
  XCTAssertEqual(call.stateLabel, "通话中")
  XCTAssertFalse(call.isIncoming)
 }
}
