import XCTest

final class DashboardUITests: XCTestCase {
    func testHomeFitsAndNumberEditingDoesNotOpenASecondKeyboard() {
        let app = XCUIApplication()
        app.launch()
        XCTAssertTrue(app.buttons["dialNumber"].waitForExistence(timeout: 8))
        let bounds = app.frame
        for key in ["1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#", "拨打电话", "openMessages"] {
            let button = app.buttons[key]
            XCTAssertTrue(button.exists, "Missing control: \(key)")
            XCTAssertTrue(bounds.contains(button.frame), "Control outside screen: \(key)")
        }
        XCTAssertEqual(app.scrollViews.count, 0)
        app.buttons["1"].tap(); app.buttons["3"].tap()
        XCTAssertEqual(app.buttons["dialNumber"].value as? String, "13")
        XCTAssertEqual(app.keyboards.count, 0)
        app.buttons["删除一位"].tap()
        XCTAssertEqual(app.buttons["dialNumber"].value as? String, "1")
        let attachment = XCTAttachment(screenshot: app.screenshot())
        attachment.name = "single-screen-home"; attachment.lifetime = .keepAlways; add(attachment)
    }
    func testNotificationPermissionAndTestBanner() {
        let app = XCUIApplication()
        app.launch()
        XCTAssertTrue(app.buttons["通知设置"].waitForExistence(timeout: 8))
        app.buttons["通知设置"].tap()
        let enable = app.buttons["开启短信通知"]
        if enable.waitForExistence(timeout: 2) {
            enable.tap()
            let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
            let allow = springboard.buttons["Allow"]
            if allow.waitForExistence(timeout: 3) { allow.tap() }
            else if springboard.buttons["允许"].exists { springboard.buttons["允许"].tap() }
        }
        let test = app.buttons["仅测试本机横幅"]
        // A partly visible cell at the home-indicator edge may report hittable
        // although its tap lands in the system gesture area. Scroll it clear.
        for _ in 0..<3 {
            if test.isHittable && test.frame.maxY < app.frame.maxY - 80 { break }
            app.swipeUp()
        }
        XCTAssertTrue(test.waitForExistence(timeout: 3))
        XCTAssertTrue(test.isEnabled)
        test.tap()
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        let banner = springboard.staticTexts["小电波 · 通知测试"]
        XCTAssertTrue(banner.waitForExistence(timeout: 8), "Scheduled local banner did not arrive")
        let attachment = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        attachment.name = "local-notification-banner"; attachment.lifetime = .keepAlways; add(attachment)
    }
}
