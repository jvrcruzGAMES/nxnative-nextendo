import Foundation
import Network
import SwiftUI
import UIKit

private struct NxShareFile: Decodable {
    let path: String
    let size: Int64
    let url: String
}

private struct NxShareManifest: Decodable {
    let titleId: String
    let name: String?
    let version: String?
    let totalSize: Int64
    let files: [NxShareFile]
}

private final class NxShareModel: NSObject, ObservableObject, URLSessionDownloadDelegate {
    enum Phase {
        case idle
        case fetchingManifest
        case wifiWarning
        case downloading
        case completed
        case failed(String)
    }

    @Published var serverURL: String
    @Published var phase: Phase = .idle
    @Published var manifest: NxShareManifest?
    @Published var currentFileName: String = ""
    @Published var currentFileIndex: Int = 0
    @Published var totalFiles: Int = 0
    @Published var downloadedBytes: Int64 = 0
    @Published var totalBytes: Int64 = 0
    @Published var isOnWiFi: Bool = true
    @Published var networkChecked: Bool = false

    let titleId: String
    let appName: String

    private let monitor = NWPathMonitor()
    private let monitorQueue = DispatchQueue(label: "org.nxnative.nxshare.net")
    private var session: URLSession!
    private var activeTask: URLSessionDownloadTask?
    private var completedBytesBeforeCurrent: Int64 = 0
    private var activeFileDestination: URL?
    private var activeFileExpectedSize: Int64 = 0
    private var pendingFiles: [NxShareFile] = []
    private var onComplete: (() -> Void)?

    private static let savedURLKey = "nxshare.serverURL"

    override init() {
        let info = Bundle.main.infoDictionary ?? [:]
        self.titleId = (info["DMTitleID"] as? String) ?? ""
        self.appName = (info["CFBundleDisplayName"] as? String) ?? "Game"
        let plistURL = info["DMNxShareURL"] as? String
        let saved = UserDefaults.standard.string(forKey: Self.savedURLKey)
        self.serverURL = saved ?? plistURL ?? "http://"
        super.init()
        let config = URLSessionConfiguration.default
        config.timeoutIntervalForRequest = 30
        config.timeoutIntervalForResource = 60 * 60 * 12
        self.session = URLSession(configuration: config, delegate: self, delegateQueue: nil)
        startMonitoring()
    }

    deinit {
        monitor.cancel()
        session.invalidateAndCancel()
    }

    private func startMonitoring() {
        monitor.pathUpdateHandler = { [weak self] path in
            DispatchQueue.main.async {
                self?.isOnWiFi = path.usesInterfaceType(.wifi)
                self?.networkChecked = true
            }
        }
        monitor.start(queue: monitorQueue)
    }

    func start(onComplete: @escaping () -> Void) {
        self.onComplete = onComplete
        let trimmed = serverURL.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.count > "http://".count, URL(string: trimmed)?.host != nil {
            fetchManifest()
        }
    }

    func fetchManifest() {
        let base = serverURL.trimmingCharacters(in: .whitespacesAndNewlines).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard !base.isEmpty, let url = URL(string: "\(base)/api/apps/\(titleId)/manifest") else {
            phase = .failed("Enter a valid nxshare server URL (e.g. http://192.168.1.10:8080).")
            return
        }
        UserDefaults.standard.set(base, forKey: Self.savedURLKey)
        serverURL = base
        phase = .fetchingManifest

        let task = session.dataTask(with: url) { [weak self] data, response, error in
            DispatchQueue.main.async {
                guard let self else { return }
                if let error {
                    self.phase = .failed("Could not reach server: \(error.localizedDescription)")
                    return
                }
                if let http = response as? HTTPURLResponse, !(200...299).contains(http.statusCode) {
                    self.phase = .failed("Server returned HTTP \(http.statusCode) for title \(self.titleId).")
                    return
                }
                guard let data else {
                    self.phase = .failed("Empty response from server.")
                    return
                }
                do {
                    let decoded = try JSONDecoder().decode(NxShareManifest.self, from: data)
                    self.manifest = decoded
                    self.totalBytes = decoded.totalSize
                    self.totalFiles = decoded.files.count
                    self.checkWiFiAndProceed()
                } catch {
                    self.phase = .failed("Invalid manifest: \(error.localizedDescription)")
                }
            }
        }
        task.resume()
    }

    func checkWiFiAndProceed() {
        if !isOnWiFi {
            phase = .wifiWarning
        } else {
            beginDownload()
        }
    }

    func proceedWithoutWiFi() {
        beginDownload()
    }

    func cancelDownload() {
        activeTask?.cancel()
        activeTask = nil
        phase = .idle
    }

    private func beginDownload() {
        guard let manifest else { return }
        phase = .downloading
        completedBytesBeforeCurrent = 0
        downloadedBytes = 0
        currentFileIndex = 0
        pendingFiles = manifest.files
        downloadNextFile()
    }

    private func documentsDirectory() -> URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    private func downloadNextFile() {
        guard !pendingFiles.isEmpty else {
            DispatchQueue.main.async {
                self.phase = .completed
                self.onComplete?()
            }
            return
        }

        let file = pendingFiles.removeFirst()
        let docDir = documentsDirectory()
        let dest = docDir.appendingPathComponent(file.path)
        let fm = FileManager.default

        if let attrs = try? fm.attributesOfItem(atPath: dest.path),
           let existingSize = attrs[.size] as? Int64,
           existingSize == file.size {
            DispatchQueue.main.async {
                self.currentFileIndex += 1
                self.completedBytesBeforeCurrent += file.size
                self.downloadedBytes = self.completedBytesBeforeCurrent
                self.downloadNextFile()
            }
            return
        }

        try? fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
        guard let url = URL(string: file.url) else {
            DispatchQueue.main.async {
                self.phase = .failed("Invalid file URL: \(file.url)")
            }
            return
        }

        DispatchQueue.main.async {
            self.currentFileIndex += 1
            self.currentFileName = file.path
            self.activeFileDestination = dest
            self.activeFileExpectedSize = file.size
        }

        let task = session.downloadTask(with: url)
        activeTask = task
        task.resume()
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        DispatchQueue.main.async {
            self.downloadedBytes = self.completedBytesBeforeCurrent + totalBytesWritten
        }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard let dest = activeFileDestination else { return }
        let fm = FileManager.default
        do {
            if fm.fileExists(atPath: dest.path) {
                try fm.removeItem(at: dest)
            }
            try fm.moveItem(at: location, to: dest)
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            var mutableDest = dest
            try? mutableDest.setResourceValues(values)
            DispatchQueue.main.async {
                self.completedBytesBeforeCurrent += self.activeFileExpectedSize
                self.downloadedBytes = self.completedBytesBeforeCurrent
                self.downloadNextFile()
            }
        } catch {
            DispatchQueue.main.async {
                self.phase = .failed("Failed to save \(dest.lastPathComponent): \(error.localizedDescription)")
            }
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error else { return }
        let nsErr = error as NSError
        if nsErr.domain == NSURLErrorDomain && nsErr.code == NSURLErrorCancelled { return }
        DispatchQueue.main.async {
            self.phase = .failed("Download failed: \(error.localizedDescription)")
        }
    }
}

private struct NxShareView: View {
    @ObservedObject var model: NxShareModel
    @State private var showWifiAlert = false

    private func formatBytes(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }

    var body: some View {
        NavigationView {
            Form {
                Section {
                    HStack {
                        Text("Game")
                        Spacer()
                        Text(model.appName).foregroundStyle(.secondary)
                    }
                    HStack {
                        Text("Title ID")
                        Spacer()
                        Text(model.titleId).font(.system(.body, design: .monospaced)).foregroundStyle(.secondary)
                    }
                    HStack {
                        Text("Connection")
                        Spacer()
                        Text(model.isOnWiFi ? "Wi-Fi" : "Not on Wi-Fi")
                            .foregroundStyle(model.isOnWiFi ? .green : .orange)
                    }
                } header: {
                    Text("Game Data Download")
                }

                Section {
                    TextField("http://192.168.1.10:8080", text: $model.serverURL)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .disabled(isBusy)
                    Button("Connect & Download") {
                        model.fetchManifest()
                    }
                    .disabled(isBusy || model.serverURL.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                } header: {
                    Text("NxShare Server")
                }

                switch model.phase {
                case .idle:
                    EmptyView()
                case .fetchingManifest:
                    Section {
                        HStack(spacing: 12) {
                            ProgressView()
                            Text("Contacting server…")
                        }
                    }
                case .wifiWarning:
                    Section {
                        VStack(alignment: .leading, spacing: 8) {
                            Label("Not connected to Wi-Fi", systemImage: "exclamationmark.triangle.fill")
                                .foregroundStyle(.orange)
                                .font(.headline)
                            Text("Downloading \(formatBytes(model.totalBytes)) over a non-Wi-Fi connection may use cellular data.")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                            HStack {
                                Button("Proceed Anyway") { model.proceedWithoutWiFi() }
                                    .buttonStyle(.borderedProminent)
                                Button("Cancel", role: .cancel) { model.cancelDownload() }
                                    .buttonStyle(.bordered)
                            }
                            .padding(.top, 4)
                        }
                        .padding(.vertical, 4)
                    }
                case .downloading:
                    Section {
                        VStack(alignment: .leading, spacing: 8) {
                            let fraction = model.totalBytes > 0 ? min(1.0, Double(model.downloadedBytes) / Double(model.totalBytes)) : 0.0
                            ProgressView(value: fraction)
                            HStack {
                                Text("File \(model.currentFileIndex) of \(model.totalFiles)")
                                Spacer()
                                Text("\(formatBytes(model.downloadedBytes)) / \(formatBytes(model.totalBytes))")
                                    .monospacedDigit()
                            }
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            if !model.currentFileName.isEmpty {
                                Text(model.currentFileName)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                            }
                            Button("Cancel", role: .destructive) { model.cancelDownload() }
                        }
                        .padding(.vertical, 4)
                    } header: {
                        Text("Downloading")
                    }
                case .completed:
                    Section {
                        Label("Download complete. Starting…", systemImage: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                    }
                case .failed(let message):
                    Section {
                        Text(message).foregroundStyle(.red)
                    } header: {
                        Text("Error")
                    }
                }
            }
            .navigationTitle("Download Game Data")
            .onChange(of: isWifiWarning) { _, warning in
                if warning { showWifiAlert = true }
            }
            .alert("Not on Wi-Fi", isPresented: $showWifiAlert) {
                Button("Proceed Anyway") { model.proceedWithoutWiFi() }
                Button("Cancel", role: .cancel) { model.cancelDownload() }
            } message: {
                Text("You are not connected to Wi-Fi. The download is \(formatBytes(model.totalBytes)). Do you want to proceed anyway?")
            }
        }
        .navigationViewStyle(.stack)
    }

    private var isBusy: Bool {
        switch model.phase {
        case .fetchingManifest, .downloading: return true
        default: return false
        }
    }

    private var isWifiWarning: Bool {
        if case .wifiWarning = model.phase { return true }
        return false
    }
}

private var activeNxShareModel: NxShareModel?

@_cdecl("dm_show_nxshare_downloader")
public func dm_show_nxshare_downloader(_ viewController: UnsafeMutableRawPointer, _ done: @escaping @convention(c) () -> Void) {
    let parent = Unmanaged<UIViewController>.fromOpaque(viewController).takeUnretainedValue()
    let model = NxShareModel()
    activeNxShareModel = model
    var host: UIHostingController<NxShareView>?
    let view = NxShareView(model: model)
    let vc = UIHostingController(rootView: view)
    vc.isModalInPresentation = true
    host = vc
    parent.present(vc, animated: true) {
        model.start {
            host?.dismiss(animated: true) {
                activeNxShareModel = nil
                done()
            }
        }
    }
}
