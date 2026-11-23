// TranslationBridge.swift
// Headless wrapper around Apple's on-device Translation framework (iOS 18+).
//
// UNTESTED: this file has never been compiled. Apple's TranslationSession is
// designed to be obtained via a SwiftUI `.translationTask` attached to a live
// view (see developer.apple.com/documentation/translation). To get a session
// without building real UI, this hosts one SwiftUI view inside a hidden,
// non-interactive UIWindow and waits for the system to hand back a session
// through that view's .translationTask callback — the same technique used by
// small open-source helpers for this exact problem. If a future iOS drops
// support for this trick, or Apple changes the view lifecycle requirements,
// this breaks and needs a real device + Xcode to debug, which I don't have
// here.
//
// KNOWN LIMITATION (confirmed from Apple's own docs, not guessed): on iOS 18
// through 25 a session created this way cannot trigger a language-pack
// download by itself. If the Arabic pack isn't already installed on the
// device (Settings > General > Language & Region > Translation Languages,
// or opened once from the Translate app), translation calls will fail.
// There is no code fix for this — it must be done once, by hand, on-device.

import UIKit
import SwiftUI
import Translation

@objc public class TranslationBridge: NSObject {
    @objc public static let shared = TranslationBridge()

    private var hostWindow: UIWindow?
    private var session: TranslationSession?
    private var sessionReady: ((TranslationSession?) -> Void)?
    private var pendingConfig: TranslationSession.Configuration?

    // Called once, early. targetCode is a BCP-47 language code ("ar" for Arabic).
    // sourceCode is normally "en" for these games; pass nil to let the system
    // try to detect it, though explicit is more reliable.
    @objc public func prepare(sourceCode: String?, targetCode: String, completion: @escaping (Bool) -> Void) {
        let source = sourceCode.map { Locale.Language(identifier: $0) }
        let target = Locale.Language(identifier: targetCode)
        pendingConfig = TranslationSession.Configuration(source: source, target: target)
        sessionReady = { session in completion(session != nil) }

        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive }) else {
            // Retry shortly if the app hasn't finished launching yet.
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
                self?.prepare(sourceCode: sourceCode, targetCode: targetCode, completion: completion)
            }
            return
        }

        let window = UIWindow(windowScene: scene)
        window.isHidden = false
        window.alpha = 0.01            // effectively invisible, but SwiftUI still runs its lifecycle
        window.isUserInteractionEnabled = false
        window.windowLevel = .normal - 1
        window.rootViewController = UIHostingController(rootView: HiddenTranslationHost(bridge: self))
        hostWindow = window
    }

    fileprivate func configuration() -> TranslationSession.Configuration? { pendingConfig }

    fileprivate func onSessionAvailable(_ s: TranslationSession) {
        session = s
        sessionReady?(s)
        sessionReady = nil
    }

    // texts: JSON-encoded array of plain-prose strings (never containing RPG
    // Maker escape codes — the JS side strips those out before calling this).
    // Calls back with a JSON-encoded array of the same length; any entry that
    // could not be translated is returned as an empty string so the caller
    // can fall back to leaving that segment untranslated.
    @objc public func translateBatch(textsJSON: String, completion: @escaping (String) -> Void) {
        guard let session = session,
              let data = textsJSON.data(using: .utf8),
              let texts = try? JSONDecoder().decode([String].self, from: data) else {
            completion("[]")
            return
        }
        Task {
            var results = [String](repeating: "", count: texts.count)
            do {
                let requests = texts.enumerated().map { TranslationSession.Request(sourceText: $1, clientIdentifier: String($0)) }
                for try await response in try await session.translate(batch: requests) {
                    if let idStr = response.clientIdentifier, let idx = Int(idStr), idx < results.count {
                        results[idx] = response.targetText
                    }
                }
            } catch {
                // Leave `results` as empty strings; the JS side treats an
                // empty translation as "keep the original text for this line".
            }
            if let out = try? JSONEncoder().encode(results), let str = String(data: out, encoding: .utf8) {
                completion(str)
            } else {
                completion("[]")
            }
        }
    }
}

private struct HiddenTranslationHost: View {
    let bridge: TranslationBridge
    @State private var config: TranslationSession.Configuration?

    var body: some View {
        Color.clear
            .onAppear { config = bridge.configuration() }
            .translationTask(config) { session in
                bridge.onSessionAvailable(session)
            }
    }
}
