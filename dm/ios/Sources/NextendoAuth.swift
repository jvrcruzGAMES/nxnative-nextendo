// Nextendo Network in-app authentication, Keychain storage and local OAuth callback server.
import Foundation
import Security
import SwiftUI
import WebKit
#if canImport(AppKit)
import AppKit
#endif
#if canImport(UIKit)
import UIKit
#endif

// MARK: - Keychain Helper

public enum NextendoKeychain {
    public static let service = "network.nextendo.auth"
    public static let account = "session"

#if os(macOS)
    private static var macAuthURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support")
        return base.appendingPathComponent("nxnative/nextendo_auth.json")
    }
#endif

    @discardableResult
    public static func save(token: String, username: String, pid: UInt64, friendCode: String = "", refreshToken: String = "") -> Bool {
        var dict: [String: Any] = [
            "token": token,
            "username": username,
            "pid": pid,
            "nex_token_at": Date().timeIntervalSince1970
        ]
        if !friendCode.isEmpty {
            dict["friend_code"] = friendCode
        }
        if !refreshToken.isEmpty {
            dict["refresh_token"] = refreshToken
        }
        guard let data = try? JSONSerialization.data(withJSONObject: dict) else { return false }

#if os(macOS)
        let url = macAuthURL
        do {
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try data.write(to: url, options: .atomic)
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
            return true
        } catch {
            return false
        }
#else
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account
        ]
        SecItemDelete(query as CFDictionary)

        let attributes: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlock,
            kSecValueData as String: data
        ]
        let status = SecItemAdd(attributes as CFDictionary, nil)
        return status == errSecSuccess
#endif
    }

    public static func load() -> (token: String, username: String, pid: UInt64, friendCode: String)? {
        guard let s = loadSession() else { return nil }
        return (s.token, s.username, s.pid, s.friendCode)
    }

    public static func loadSession() -> (token: String, username: String, pid: UInt64, friendCode: String, refreshToken: String, nexTokenAt: TimeInterval)? {
#if os(macOS)
        guard let data = try? Data(contentsOf: macAuthURL) else { return nil }
#else
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess, let data = result as? Data else { return nil }
#endif

        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let token = json["token"] as? String, !token.isEmpty else {
            return nil
        }
        let username = (json["username"] as? String) ?? "Player"
        let pid: UInt64
        if let p = json["pid"] as? UInt64 {
            pid = p
        } else if let p = json["pid"] as? Int64 {
            pid = UInt64(p)
        } else if let p = json["pid"] as? Int {
            pid = UInt64(p)
        } else if let s = json["pid"] as? String, let p = UInt64(s) {
            pid = p
        } else {
            pid = 0xcafe
        }
        let friendCode = (json["friend_code"] as? String) ?? ""
        let refreshToken = (json["refresh_token"] as? String) ?? ""
        let nexTokenAt = (json["nex_token_at"] as? TimeInterval) ?? 0
        return (token, username, pid, friendCode, refreshToken, nexTokenAt)
    }

    @discardableResult
    public static func delete() -> Bool {
#if os(macOS)
        let url = macAuthURL
        if !FileManager.default.fileExists(atPath: url.path) { return true }
        return (try? FileManager.default.removeItem(at: url)) != nil
#else
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account
        ]
        let st = SecItemDelete(query as CFDictionary)
        return st == errSecSuccess || st == errSecItemNotFound
#endif
    }
}

// MARK: - Local HTTP Callback Server (Port 47823)

final class NextendoCallbackServer {
    private var listenFd: Int32 = -1
    private var isRunning = false
    private let queue = DispatchQueue(label: "network.nextendo.callback", qos: .userInitiated)

    func start(port: UInt16 = 47823, onResult: @escaping (String?, String?, String?) -> Void) -> Bool {
        stop()
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return false }

        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        #if os(macOS) || os(iOS)
        var noSigPipe: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, socklen_t(MemoryLayout<Int32>.size))
        #endif

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.stride)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")

        let bindRes = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.stride))
            }
        }
        guard bindRes == 0 else {
            close(fd)
            return false
        }

        guard listen(fd, 5) == 0 else {
            close(fd)
            return false
        }

        listenFd = fd
        isRunning = true

        queue.async { [weak self] in
            self?.acceptLoop(onResult: onResult)
        }
        return true
    }

    func stop() {
        isRunning = false
        if listenFd >= 0 {
            close(listenFd)
            listenFd = -1
        }
    }

    private func acceptLoop(onResult: @escaping (String?, String?, String?) -> Void) {
        while isRunning && listenFd >= 0 {
            var clientAddr = sockaddr_in()
            var clientLen = socklen_t(MemoryLayout<sockaddr_in>.stride)
            let clientFd = withUnsafeMutablePointer(to: &clientAddr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    accept(listenFd, $0, &clientLen)
                }
            }
            if clientFd < 0 {
                if !isRunning { break }
                continue
            }

            var buf = [UInt8](repeating: 0, count: 4096)
            let n = read(clientFd, &buf, buf.count - 1)
            if n > 0 {
                let req = String(decoding: buf.prefix(n), as: UTF8.self)
                if let firstLine = req.split(separator: "\r\n").first,
                   let getPart = firstLine.split(separator: " ").dropFirst().first {
                    let path = String(getPart)
                    let (code, token, error) = Self.parseQueryParams(path)

                    let html = """
                    <!DOCTYPE html>
                    <html>
                    <head><meta charset="utf-8"><title>Nextendo Login</title>
                    <style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;text-align:center;padding:50px;background:#18181b;color:#f4f4f5;}</style>
                    </head>
                    <body>
                    <h2>Nextendo Authentication \(error != nil ? "Failed" : "Successful")!</h2>
                    <p>\(error != nil ? "Error: " + (error ?? "") : "You may now close this tab and return to the game.")</p>
                    <script>
                    if (window.location.hash) {
                        var p = new URLSearchParams(window.location.hash.substring(1));
                        var t = p.get('access_token') || p.get('token');
                        if (t) { fetch('/callback?token=' + encodeURIComponent(t)); }
                    }
                    </script>
                    </body>
                    </html>
                    """
                    let statusLine = error != nil ? "HTTP/1.1 400 Bad Request" : "HTTP/1.1 200 OK"
                    let resp = "\(statusLine)\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: \(html.utf8.count)\r\n\r\n" + html
                    _ = resp.withCString { write(clientFd, $0, strlen($0)) }
                    close(clientFd)

                    if code != nil || token != nil || error != nil {
                        stop()
                        onResult(code, token, error)
                        return
                    }
                }
            }
            close(clientFd)
        }
    }

    static func parseQueryParams(_ urlStr: String) -> (code: String?, token: String?, error: String?) {
        guard let queryStart = urlStr.firstIndex(of: "?") else { return (nil, nil, nil) }
        let queryString = String(urlStr[urlStr.index(after: queryStart)...])
        var code: String?
        var token: String?
        var error: String?
        for pair in queryString.split(separator: "&") {
            let parts = pair.split(separator: "=", maxSplits: 1).map(String.init)
            if parts.count == 2 {
                let key = parts[0]
                let val = parts[1].removingPercentEncoding ?? parts[1]
                if key == "code" { code = val }
                else if key == "token" || key == "access_token" { token = val }
                else if key == "error" { error = val }
            }
        }
        return (code, token, error)
    }
}

// MARK: - Nextendo Authentication Controller

public final class NextendoAuth: ObservableObject {
    public static let shared = NextendoAuth()

    public static let clientId = "nxc_cUHxqMhkXXSA"
    public static let redirectUri = "http://127.0.0.1:47823/callback"
    public static let authEndpoint = "https://nextendo.network/api/oauth/authorize"
    public static let tokenEndpoint = "https://nextendo.network/api/oauth/token"
    public static let nexTokenEndpoint = "https://nextendo.network/api/nex-token"
    public static let userEndpoint = "https://nextendo.network/oauth/userinfo"

    @Published public private(set) var isLoggedIn = false
    @Published public private(set) var username = "Player"
    @Published public private(set) var pid: UInt64 = 0
    @Published public private(set) var friendCode = ""
    @Published public private(set) var token = ""
    @Published public private(set) var isLoggingIn = false
    @Published public var errorMessage: String?
    @Published public var showAuthSheet = false
    @Published public var authURL: URL? = nil

    public var pidString: String {
        pid == 0 ? "-" : String(format: "0x%016llX (%llu)", pid, pid)
    }

    private let callbackServer = NextendoCallbackServer()
    private var activeVerifier = ""

    private init() {
        checkLogin()
    }

    public func checkLogin() {
        if let creds = NextendoKeychain.loadSession() {
            token = creds.token
            username = creds.username
            pid = creds.pid
            friendCode = creds.friendCode
            isLoggedIn = true
            setenv("NEXTENDO_TOKEN", creds.token, 1)
            setenv("NEXTENDO_USER", creds.username, 1)
            setenv("NEXTENDO_PID", String(creds.pid), 1)
            if !creds.friendCode.isEmpty {
                setenv("NEXTENDO_FRIEND_CODE", creds.friendCode, 1)
            }
            let isStale = !creds.token.hasPrefix("nx2") || (Date().timeIntervalSince1970 - creds.nexTokenAt > 20 * 3600)
            if !creds.refreshToken.isEmpty && isStale {
                refreshNexToken(refreshToken: creds.refreshToken)
            } else if creds.friendCode.isEmpty || creds.username == "Player" || creds.pid == 0xcafe {
                fetchUserProfileAndSave(token: creds.token)
            }
        } else {
            // Check if environment has it
            if let envTok = getenv("NEXTENDO_TOKEN"), envTok.pointee != 0 {
                token = String(cString: envTok)
                username = getenv("NEXTENDO_USER").map { String(cString: $0) } ?? "Player"
                pid = getenv("NEXTENDO_PID").flatMap { UInt64(String(cString: $0)) } ?? 0xcafe
                friendCode = getenv("NEXTENDO_FRIEND_CODE").map { String(cString: $0) } ?? ""
                isLoggedIn = true
            } else {
                token = ""
                username = "Player"
                pid = 0
                friendCode = ""
                isLoggedIn = false
            }
        }
    }

    public func startLogin() {
        cancelLogin()
        errorMessage = nil

        let (verifier, challenge) = generatePKCE()
        activeVerifier = verifier

        let ok = callbackServer.start(port: 47823) { [weak self] code, token, error in
            DispatchQueue.main.async {
                self?.handleCallback(code: code, token: token, error: error)
            }
        }

        guard ok else {
            errorMessage = "Failed to bind callback server on 127.0.0.1:47823"
            return
        }

        isLoggingIn = true

        let state = UUID().uuidString
        var components = URLComponents(string: Self.authEndpoint)!
        components.queryItems = [
            URLQueryItem(name: "client_id", value: Self.clientId),
            URLQueryItem(name: "response_type", value: "code"),
            URLQueryItem(name: "redirect_uri", value: Self.redirectUri),
            URLQueryItem(name: "scope", value: "identity presence profil friends game.matchmaking"),
            URLQueryItem(name: "code_challenge", value: challenge),
            URLQueryItem(name: "code_challenge_method", value: "S256"),
            URLQueryItem(name: "state", value: state)
        ]

        if let url = components.url {
            self.authURL = url
            self.showAuthSheet = true
        }
    }

    public func cancelLogin() {
        callbackServer.stop()
        isLoggingIn = false
        showAuthSheet = false
        activeVerifier = ""
        authURL = nil
    }

    public func logout() {
        cancelLogin()
        NextendoKeychain.delete()
        unsetenv("NEXTENDO_TOKEN")
        unsetenv("NEXTENDO_USER")
        unsetenv("NEXTENDO_PID")
        unsetenv("NEXTENDO_FRIEND_CODE")
        token = ""
        username = "Player"
        pid = 0
        friendCode = ""
        isLoggedIn = false
        token = ""
        username = "Player"
        pid = 0
        isLoggedIn = false
        errorMessage = nil
    }

    public func handleCallbackURL(_ url: URL) {
        let (code, token, error) = NextendoCallbackServer.parseQueryParams(url.absoluteString)
        handleCallback(code: code, token: token, error: error)
    }

    private func handleCallback(code: String?, token: String?, error: String?) {
        isLoggingIn = false
        showAuthSheet = false
        authURL = nil
        if let error = error {
            errorMessage = "Login failed: \(error)"
            return
        }

        if let directToken = token, !directToken.isEmpty {
            fetchUserProfileAndSave(token: directToken)
            return
        }

        guard let code = code, !code.isEmpty else {
            errorMessage = "No authorization code received"
            return
        }

        exchangeCodeForToken(code: code)
    }

    private func exchangeCodeForToken(code: String) {
        guard let url = URL(string: Self.tokenEndpoint) else { return }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        req.setValue(Self.clientId, forHTTPHeaderField: "X-Nextendo-Client-Id")
        req.setValue("nxnative/1.0", forHTTPHeaderField: "User-Agent")

        let params: [String: String] = [
            "grant_type": "authorization_code",
            "client_id": Self.clientId,
            "code": code,
            "redirect_uri": Self.redirectUri,
            "code_verifier": activeVerifier
        ]
        let bodyString = params.map { "\($0.key)=\($0.value.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? $0.value)" }.joined(separator: "&")
        req.httpBody = bodyString.data(using: .utf8)

        URLSession.shared.dataTask(with: req) { [weak self] data, _, error in
            if let error = error {
                DispatchQueue.main.async { self?.errorMessage = "Token error: \(error.localizedDescription)" }
                return
            }
            guard let data = data,
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                DispatchQueue.main.async { self?.errorMessage = "Invalid response from Nextendo" }
                return
            }

            if let nexTok = json["nex_token"] as? String, !nexTok.isEmpty {
                let acct = json["account"] as? [String: Any]
                let username = (acct?["username"] as? String) ?? (json["username"] as? String) ?? "Player"
                let friendCode = (acct?["friend_code"] as? String) ?? (json["friend_code"] as? String) ?? ""
                let pid = Self.parsePid(acct?["pid"] ?? json["pid"] ?? json["user_id"])
                let refreshTok = (json["refresh_token"] as? String) ?? ""
                DispatchQueue.main.async {
                    self?.saveAndApply(token: nexTok, username: username, pid: pid, friendCode: friendCode, refreshToken: refreshTok)
                }
                return
            }

            let accessToken = (json["access_token"] as? String) ?? (json["token"] as? String)
            guard let tok = accessToken, !tok.isEmpty else {
                let errDesc = (json["error_description"] as? String) ?? (json["error"] as? String) ?? "Token missing"
                DispatchQueue.main.async { self?.errorMessage = "Auth error: \(errDesc)" }
                return
            }
            let refreshTok = (json["refresh_token"] as? String) ?? ""
            self?.fetchNexTokenAndSave(accessToken: tok, refreshToken: refreshTok)
        }.resume()
    }

    private static func parsePid(_ val: Any?) -> UInt64 {
        if let p = val as? UInt64 { return p }
        if let p = val as? Int64 { return UInt64(p) }
        if let p = val as? Int { return UInt64(p) }
        if let s = val as? String, let p = UInt64(s) { return p }
        return 0xcafe
    }

    private func refreshNexToken(refreshToken: String) {
        guard let url = URL(string: Self.tokenEndpoint) else { return }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        req.setValue(Self.clientId, forHTTPHeaderField: "X-Nextendo-Client-Id")
        req.setValue("nxnative/1.0", forHTTPHeaderField: "User-Agent")

        let params: [(String, String)] = [
            ("grant_type", "refresh_token"),
            ("refresh_token", refreshToken),
            ("client_id", Self.clientId),
        ]
        let bodyStr = params.map {
            let k = $0.0.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? $0.0
            let v = $0.1.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? $0.1
            return "\(k)=\(v)"
        }.joined(separator: "&")
        req.httpBody = bodyStr.data(using: .utf8)

        URLSession.shared.dataTask(with: req) { [weak self] data, _, _ in
            guard let data = data,
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let accessToken = (json["access_token"] as? String) ?? (json["token"] as? String),
                  !accessToken.isEmpty else {
                return
            }
            let newRefresh = (json["refresh_token"] as? String) ?? refreshToken
            self?.fetchNexTokenAndSave(accessToken: accessToken, refreshToken: newRefresh)
        }.resume()
    }

    private func fetchNexTokenAndSave(accessToken: String, refreshToken: String = "") {
        guard let url = URL(string: Self.nexTokenEndpoint) else { return }
        var req = URLRequest(url: url)
        req.setValue("Bearer \(accessToken)", forHTTPHeaderField: "Authorization")
        req.setValue(Self.clientId, forHTTPHeaderField: "X-Nextendo-Client-Id")
        req.setValue("nxnative/1.0", forHTTPHeaderField: "User-Agent")

        URLSession.shared.dataTask(with: req) { [weak self] data, _, error in
            if let error = error {
                DispatchQueue.main.async { self?.errorMessage = "NEX token error: \(error.localizedDescription)" }
                return
            }
            guard let data = data,
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let nexTok = json["nex_token"] as? String, !nexTok.isEmpty else {
                let msg = (data.flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }?["error"] as? String) ?? "Failed to obtain NEX token"
                DispatchQueue.main.async { self?.errorMessage = msg }
                return
            }
            let user = (json["username"] as? String) ?? "Player"
            let fc = (json["friend_code"] as? String) ?? ""
            let pid = Self.parsePid(json["pid"])
            DispatchQueue.main.async {
                self?.saveAndApply(token: nexTok, username: user, pid: pid, friendCode: fc, refreshToken: refreshToken)
            }
        }.resume()
    }

    private func fetchUserProfileAndSave(token: String) {
        guard let url = URL(string: Self.userEndpoint) else {
            saveAndApply(token: token, username: "Player", pid: 0xcafe)
            return
        }
        var req = URLRequest(url: url)
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        req.setValue("nxnative/1.0", forHTTPHeaderField: "User-Agent")

        URLSession.shared.dataTask(with: req) { [weak self] data, _, _ in
            var user = "Player"
            var pid: UInt64 = 0xcafe
            var fc = ""
            if let data = data,
               let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                if let u = json["username"] as? String, !u.isEmpty { user = u }
                else if let d = json["display_name"] as? String, !d.isEmpty { user = d }
                if let f = json["friend_code"] as? String { fc = f }
                if let p = json["pid"] as? UInt64 { pid = p }
                else if let p = json["id"] as? UInt64 { pid = p }
                else if let p = json["pid"] as? Int64 { pid = UInt64(p) }
                else if let p = json["pid"] as? Int { pid = UInt64(p) }
            }
            DispatchQueue.main.async {
                self?.saveAndApply(token: token, username: user, pid: pid, friendCode: fc)
            }
        }.resume()
    }

    private func saveAndApply(token: String, username: String, pid: UInt64, friendCode: String = "", refreshToken: String = "") {
        NextendoKeychain.save(token: token, username: username, pid: pid, friendCode: friendCode, refreshToken: refreshToken)
        self.token = token
        self.username = username
        self.pid = pid
        self.friendCode = friendCode
        self.isLoggedIn = true
        self.errorMessage = nil

        setenv("NEXTENDO_TOKEN", token, 1)
        setenv("NEXTENDO_USER", username, 1)
        setenv("NEXTENDO_PID", String(pid), 1)
        if !friendCode.isEmpty {
            setenv("NEXTENDO_FRIEND_CODE", friendCode, 1)
        }
    }

    private func generatePKCE() -> (verifier: String, challenge: String) {
        var bytes = [UInt8](repeating: 0, count: 32)
        _ = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        let verifier = Data(bytes).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")

        let hash = sha256(Data(verifier.utf8))
        let challenge = hash.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")

        return (verifier, challenge)
    }

    private func sha256(_ data: Data) -> Data {
        var hash = [UInt8](repeating: 0, count: 32)
        data.withUnsafeBytes {
            _ = CC_SHA256($0.baseAddress, CC_LONG(data.count), &hash)
        }
        return Data(hash)
    }
}

// In Swift without CommonCrypto module header:
@_silgen_name("CC_SHA256")
private func CC_SHA256(_ data: UnsafeRawPointer?, _ len: UInt32, _ md: UnsafeMutablePointer<UInt8>?) -> UnsafeMutablePointer<UInt8>?
private typealias CC_LONG = UInt32

// MARK: - SwiftUI Section View

public struct NextendoSection: View {
    @ObservedObject var auth = NextendoAuth.shared
    @State private var copiedFriendCode = false

    public init() {}

    public var body: some View {
        Section {
            if auth.isLoggedIn {
                HStack {
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Connected to Nextendo").font(.body.weight(.medium))
                        Text(auth.username)
                            .font(.callout)
                            .foregroundStyle(.secondary)
                        Text(auth.friendCode.isEmpty ? "Friend Code: —" : "Friend Code: \(auth.friendCode)")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                }
                HStack(spacing: 12) {
                    Button("Log Out", role: .destructive) {
                        auth.logout()
                    }
                    Spacer()
                    if !auth.friendCode.isEmpty {
                        Button(copiedFriendCode ? "Copied!" : "Copy Friend Code") {
                            #if canImport(AppKit)
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(auth.friendCode, forType: .string)
                            #elseif canImport(UIKit)
                            UIPasteboard.general.string = auth.friendCode
                            #endif
                            copiedFriendCode = true
                            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                                copiedFriendCode = false
                            }
                        }
                        .buttonStyle(.bordered)
                    }
                }
            } else if auth.isLoggingIn {
                VStack(alignment: .leading, spacing: 8) {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small)
                        Text("Nextendo login in progress…")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }
                    Button("Cancel") {
                        auth.cancelLogin()
                    }
                    .font(.callout)
                }
            } else {
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text("Nextendo Network")
                        Spacer()
                        Text("Not Logged In").foregroundStyle(.secondary)
                    }
                    if let err = auth.errorMessage {
                        Text(err).font(.caption).foregroundStyle(.red)
                    }
                }
                Button("Log In with Nextendo") {
                    auth.startLogin()
                }
            }
        } footer: {
            Text(auth.isLoggedIn
                 ? "Online features are enabled for this session."
                 : "Log in with your Nextendo account to enable online features. If not logged in, online features are disabled.")
        }
        .sheet(isPresented: $auth.showAuthSheet, onDismiss: {
            if auth.isLoggingIn {
                auth.cancelLogin()
            }
        }) {
            if auth.authURL != nil {
                NextendoAuthSheetView(auth: auth)
            }
        }
    }
}

// MARK: - In-App WebKit Authentication View

#if canImport(UIKit)
public struct NextendoWebKitView: UIViewRepresentable {
    @ObservedObject var auth: NextendoAuth

    public func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        let webView = WKWebView(frame: .zero, configuration: config)
        webView.navigationDelegate = context.coordinator
        if let url = auth.authURL {
            webView.load(URLRequest(url: url))
        }
        return webView
    }

    public func updateUIView(_ uiView: WKWebView, context: Context) {}

    public func makeCoordinator() -> Coordinator {
        Coordinator(auth: auth)
    }

    public class Coordinator: NSObject, WKNavigationDelegate {
        var auth: NextendoAuth

        init(auth: NextendoAuth) {
            self.auth = auth
        }

        public func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
            if let url = navigationAction.request.url, url.absoluteString.hasPrefix(NextendoAuth.redirectUri) {
                auth.handleCallbackURL(url)
                decisionHandler(.cancel)
                return
            }
            decisionHandler(.allow)
        }
    }
}
#elseif canImport(AppKit)
public struct NextendoWebKitView: NSViewRepresentable {
    @ObservedObject var auth: NextendoAuth

    public func makeNSView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        let webView = WKWebView(frame: .zero, configuration: config)
        webView.navigationDelegate = context.coordinator
        if let url = auth.authURL {
            webView.load(URLRequest(url: url))
        }
        return webView
    }

    public func updateNSView(_ nsView: WKWebView, context: Context) {}

    public func makeCoordinator() -> Coordinator {
        Coordinator(auth: auth)
    }

    public class Coordinator: NSObject, WKNavigationDelegate {
        var auth: NextendoAuth

        init(auth: NextendoAuth) {
            self.auth = auth
        }

        public func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
            if let url = navigationAction.request.url, url.absoluteString.hasPrefix(NextendoAuth.redirectUri) {
                auth.handleCallbackURL(url)
                decisionHandler(.cancel)
                return
            }
            decisionHandler(.allow)
        }
    }
}
#endif

public struct NextendoAuthSheetView: View {
    @ObservedObject var auth: NextendoAuth
    @Environment(\.dismiss) private var dismiss

    public var body: some View {
        NavigationStack {
            NextendoWebKitView(auth: auth)
                .navigationTitle("Sign in with Nextendo")
                #if os(iOS)
                .navigationBarTitleDisplayMode(.inline)
                #endif
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Cancel") {
                            auth.cancelLogin()
                            dismiss()
                        }
                    }
                }
        }
        #if os(macOS)
        .frame(minWidth: 500, minHeight: 600)
        #endif
    }
}
