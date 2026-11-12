// swift-tools-version: 5.9
import PackageDescription
let package = Package(name: "ModuleCore", products: [.library(name: "ModuleCore", targets: ["ModuleCore"])], targets: [.target(name: "ModuleCore"), .testTarget(name: "ModuleCoreTests", dependencies: ["ModuleCore"])])
