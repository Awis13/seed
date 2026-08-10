import AppKit
import ApplicationServices
import Foundation

private let codexBundleID = "com.openai.codex"

enum CompanionFailure: Error, CustomStringConvertible {
    case usage(String)
    case accessibility
    case codexNotRunning
    case invalidThread
    case invalidURL
    case taskProofNotFound
    case noFocusedWindow
    case composerNotFound
    case promptNotVerified
    case submitControlNotFound
    case accessibilityCall(String, AXError)

    var description: String {
        switch self {
        case .usage(let text): return text
        case .accessibility: return "Accessibility access is not granted"
        case .codexNotRunning: return "Codex Desktop is not running"
        case .invalidThread: return "Invalid Codex task id"
        case .invalidURL: return "Could not construct the Codex task URL"
        case .taskProofNotFound: return "Could not prove the configured Codex task in the focused window"
        case .noFocusedWindow: return "Could not prove the focused Codex window"
        case .composerNotFound: return "Could not find a writable Codex composer"
        case .promptNotVerified: return "Could not verify the prompt in the Codex composer"
        case .submitControlNotFound: return "Could not find a semantic Codex submit action"
        case .accessibilityCall(let operation, let error):
            return "Accessibility \(operation) failed (\(error.rawValue))"
        }
    }
}

func taskURL(threadID: String, prompt: String) throws -> URL {
    guard UUID(uuidString: threadID) != nil else {
        throw CompanionFailure.invalidThread
    }
    var parts = URLComponents()
    parts.scheme = "codex"
    parts.host = "threads"
    parts.path = "/\(threadID)"
    parts.queryItems = [URLQueryItem(name: "prompt", value: prompt)]
    guard let url = parts.url else { throw CompanionFailure.invalidURL }
    return url
}

func attribute(_ element: AXUIElement, _ name: CFString) -> AnyObject? {
    var value: CFTypeRef?
    guard AXUIElementCopyAttributeValue(element, name, &value) == .success else { return nil }
    return value
}

func stringAttribute(_ element: AXUIElement, _ name: CFString) -> String {
    return attribute(element, name) as? String ?? ""
}

func children(_ element: AXUIElement) -> [AXUIElement] {
    return attribute(element, kAXChildrenAttribute as CFString) as? [AXUIElement] ?? []
}

func descendants(_ root: AXUIElement, limit: Int = 4_000) -> [AXUIElement] {
    var result: [AXUIElement] = []
    var queue: [AXUIElement] = [root]
    while !queue.isEmpty && result.count < limit {
        let current = queue.removeFirst()
        result.append(current)
        queue.append(contentsOf: children(current))
    }
    return result
}

func isValueSettable(_ element: AXUIElement) -> Bool {
    var settable = DarwinBoolean(false)
    return AXUIElementIsAttributeSettable(
        element, kAXValueAttribute as CFString, &settable
    ) == .success && settable.boolValue
}

func semanticText(_ element: AXUIElement) -> String {
    return [kAXIdentifierAttribute, kAXTitleAttribute, kAXDescriptionAttribute,
            kAXHelpAttribute, kAXRoleDescriptionAttribute]
        .map { stringAttribute(element, $0 as CFString).lowercased() }
        .joined(separator: " ")
}

func normalizedWhitespace(_ value: String) -> String {
    return value.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
}

func provesTask(window: AXUIElement, taskTitle: String) -> Bool {
    let expected = normalizedWhitespace(taskTitle)
    guard !expected.isEmpty else { return false }
    let title = normalizedWhitespace(stringAttribute(window, kAXTitleAttribute as CFString))
    let document = normalizedWhitespace(
        stringAttribute(window, kAXDocumentAttribute as CFString)
    )
    return title == expected || document == expected
}

func hasAction(_ element: AXUIElement, _ action: CFString) -> Bool {
    var names: CFArray?
    guard AXUIElementCopyActionNames(element, &names) == .success,
          let actions = names as? [String] else { return false }
    return actions.contains(action as String)
}

func focusedWindow(for application: AXUIElement) throws -> AXUIElement {
    var value: CFTypeRef?
    let error = AXUIElementCopyAttributeValue(
        application, kAXFocusedWindowAttribute as CFString, &value
    )
    guard error == .success, let window = value else {
        throw CompanionFailure.noFocusedWindow
    }
    return unsafeBitCast(window, to: AXUIElement.self)
}

func findComposer(in window: AXUIElement, prompt: String) throws -> AXUIElement {
    let all = descendants(window)
    let writable = all.filter {
        let role = stringAttribute($0, kAXRoleAttribute as CFString)
        return (role == (kAXTextAreaRole as String) || role == (kAXTextFieldRole as String))
            && isValueSettable($0)
    }
    let semantic = writable.filter {
        let label = semanticText($0)
        return label.contains("prompt") || label.contains("message") || label.contains("composer")
            || label.contains("chat input")
    }
    let exact = semantic.filter {
        stringAttribute($0, kAXValueAttribute as CFString) == prompt
    }
    if exact.count == 1, let composer = exact.first {
        return composer
    }
    throw CompanionFailure.composerNotFound
}

func parent(_ element: AXUIElement) -> AXUIElement? {
    guard let value = attribute(element, kAXParentAttribute as CFString) else { return nil }
    return unsafeBitCast(value, to: AXUIElement.self)
}

func associatedSubmitButton(composer: AXUIElement) -> AXUIElement? {
    var ancestor = parent(composer)
    for _ in 0..<2 {
        guard let container = ancestor else { break }
        let role = stringAttribute(container, kAXRoleAttribute as CFString)
        if role == (kAXWindowRole as String) { return nil }
        let buttons = children(container).filter { element in
            let label = semanticText(element)
            return stringAttribute(element, kAXRoleAttribute as CFString) == (kAXButtonRole as String)
                && hasAction(element, kAXPressAction as CFString)
                && (label.contains("send") || label.contains("submit"))
        }
        if buttons.count == 1 {
            return buttons.first
        }
        ancestor = parent(container)
    }
    return nil
}

func submit(composer: AXUIElement) throws {
    if hasAction(composer, kAXConfirmAction as CFString) {
        let error = AXUIElementPerformAction(composer, kAXConfirmAction as CFString)
        guard error == .success else {
            throw CompanionFailure.accessibilityCall("confirm prompt", error)
        }
        return
    }
    guard let button = associatedSubmitButton(composer: composer) else {
        throw CompanionFailure.submitControlNotFound
    }
    let error = AXUIElementPerformAction(button, kAXPressAction as CFString)
    guard error == .success else {
        throw CompanionFailure.accessibilityCall("press submit", error)
    }
}

func openAndSubmit(threadID: String, taskTitle: String, prompt: String) throws {
    guard AXIsProcessTrusted() else { throw CompanionFailure.accessibility }
    guard let app = NSRunningApplication.runningApplications(
        withBundleIdentifier: codexBundleID
    ).first else { throw CompanionFailure.codexNotRunning }
    let url = try taskURL(threadID: threadID, prompt: prompt)
    guard NSWorkspace.shared.open(url) else { throw CompanionFailure.invalidURL }
    app.activate()
    let application = AXUIElementCreateApplication(app.processIdentifier)
    let deadline = Date().addingTimeInterval(12)
    var lastError: Error = CompanionFailure.noFocusedWindow
    while Date() < deadline {
        do {
            let window = try focusedWindow(for: application)
            guard provesTask(window: window, taskTitle: taskTitle) else {
                throw CompanionFailure.taskProofNotFound
            }
            let composer = try findComposer(in: window, prompt: prompt)
            guard stringAttribute(composer, kAXValueAttribute as CFString) == prompt else {
                throw CompanionFailure.promptNotVerified
            }
            try submit(composer: composer)
            print("submitted \(threadID)")
            return
        } catch {
            lastError = error
            Thread.sleep(forTimeInterval: 0.2)
        }
    }
    throw lastError
}

func argument(_ name: String, in arguments: [String]) -> String? {
    guard let index = arguments.firstIndex(of: name), index + 1 < arguments.count else {
        return nil
    }
    return arguments[index + 1]
}

func run() throws {
    let arguments = Array(CommandLine.arguments.dropFirst())
    guard let command = arguments.first else {
        throw CompanionFailure.usage("Usage: codex-pager-companion check|request-accessibility|url|submit")
    }
    switch command {
    case "check":
        guard AXIsProcessTrusted() else { throw CompanionFailure.accessibility }
        print("accessibility granted")
    case "request-accessibility":
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        if !AXIsProcessTrustedWithOptions(options) {
            throw CompanionFailure.accessibility
        }
        print("accessibility granted")
    case "url":
        guard let thread = argument("--thread", in: arguments),
              let prompt = argument("--prompt", in: arguments) else {
            throw CompanionFailure.usage("url requires --thread and --prompt")
        }
        print(try taskURL(threadID: thread, prompt: prompt).absoluteString)
    case "submit":
        guard let thread = argument("--thread", in: arguments),
              let taskTitle = argument("--task-title", in: arguments),
              let prompt = argument("--prompt", in: arguments) else {
            throw CompanionFailure.usage(
                "submit requires --thread, --task-title, and --prompt"
            )
        }
        try openAndSubmit(threadID: thread, taskTitle: taskTitle, prompt: prompt)
    default:
        throw CompanionFailure.usage("Unknown command: \(command)")
    }
}

do {
    try run()
} catch {
    FileHandle.standardError.write(Data("codex-pager-companion: \(error)\n".utf8))
    switch error {
    case CompanionFailure.usage(_), CompanionFailure.invalidThread, CompanionFailure.invalidURL:
        exit(64)
    default:
        exit(1)
    }
}
