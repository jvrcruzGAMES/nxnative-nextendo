import Compression
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
    let nxshareVersion: Int?
    let totalSize: Int64
    let uncompressedSize: Int64?
    let archive: NxShareFile?
    let files: [NxShareFile]?
}

private final class StreamingTarExtractor {
    private let destRoot: URL
    private var buffer = Data()
    private var activeFile: FileHandle?
    private var activeDest: URL?
    private var remainingFileBytes: Int64 = 0
    private var paddingBytes: Int = 0
    private var paxPath: String?
    private var paxSize: Int64?
    private var gnuLongName: String?
    private var pendingSpecialType: UInt8?
    private var specialData = Data()
    private var finished = false
    private(set) var extractedBytes: Int64 = 0

    init(destination: URL) {
        self.destRoot = destination
    }

    func feed(_ chunk: Data) throws {
        if finished || chunk.isEmpty { return }
        buffer.append(chunk)

        while !finished {
            if let special = pendingSpecialType {
                let need = Int(remainingFileBytes)
                if need > 0 {
                    let take = min(need, buffer.count)
                    if take == 0 { return }
                    specialData.append(buffer.prefix(take))
                    buffer.removeFirst(take)
                    remainingFileBytes -= Int64(take)
                    if remainingFileBytes > 0 { return }
                }
                if paddingBytes > 0 {
                    if buffer.count < paddingBytes { return }
                    buffer.removeFirst(paddingBytes)
                    paddingBytes = 0
                }
                if special == UInt8(ascii: "x") {
                    parsePax(specialData)
                } else if special == UInt8(ascii: "L") {
                    if let s = String(data: specialData, encoding: .utf8) {
                        gnuLongName = s.trimmingCharacters(in: CharacterSet(charactersIn: "\0\n"))
                    }
                }
                specialData.removeAll(keepingCapacity: true)
                pendingSpecialType = nil
                continue
            }

            if remainingFileBytes > 0 {
                let take = Int(min(remainingFileBytes, Int64(buffer.count)))
                if take == 0 { return }
                try activeFile?.write(contentsOf: buffer.prefix(take))
                buffer.removeFirst(take)
                remainingFileBytes -= Int64(take)
                extractedBytes += Int64(take)
                if remainingFileBytes == 0 {
                    try finishActiveFile()
                } else {
                    return
                }
            }

            if paddingBytes > 0 {
                if buffer.count < paddingBytes { return }
                buffer.removeFirst(paddingBytes)
                paddingBytes = 0
            }

            if buffer.count < 512 { return }
            let header = Data(buffer.prefix(512))
            buffer.removeFirst(512)

            if header.allSatisfy({ $0 == 0 }) {
                finished = true
                return
            }

            let rawName = cString(header, offset: 0, length: 100)
            let prefix = cString(header, offset: 345, length: 155)
            let combined = prefix.isEmpty ? rawName : "\(prefix)/\(rawName)"
            let typeFlag = header[156]
            let headerSize = parseTarSize(header, offset: 124, length: 12)

            if typeFlag == UInt8(ascii: "x") || typeFlag == UInt8(ascii: "g") || typeFlag == UInt8(ascii: "L") {
                pendingSpecialType = typeFlag
                specialData.removeAll(keepingCapacity: true)
                remainingFileBytes = headerSize
                let rem = Int(headerSize % 512)
                paddingBytes = rem == 0 ? 0 : (512 - rem)
                continue
            }

            let finalName = paxPath ?? gnuLongName ?? combined
            let finalSize = paxSize ?? headerSize
            paxPath = nil
            paxSize = nil
            gnuLongName = nil

            let cleanRel = sanitizePath(finalName)
            let rem = Int(finalSize % 512)
            let pad = rem == 0 ? 0 : (512 - rem)

            if typeFlag == UInt8(ascii: "5") {
                if !cleanRel.isEmpty {
                    let dirURL = destRoot.appendingPathComponent(cleanRel, isDirectory: true)
                    try FileManager.default.createDirectory(at: dirURL, withIntermediateDirectories: true)
                }
                paddingBytes = pad
                continue
            }

            if typeFlag == 0 || typeFlag == UInt8(ascii: "0") {
                if cleanRel.isEmpty {
                    remainingFileBytes = finalSize
                    paddingBytes = pad
                    continue
                }
                let outURL = destRoot.appendingPathComponent(cleanRel)
                try FileManager.default.createDirectory(at: outURL.deletingLastPathComponent(), withIntermediateDirectories: true)
                if FileManager.default.fileExists(atPath: outURL.path) {
                    try FileManager.default.removeItem(at: outURL)
                }
                FileManager.default.createFile(atPath: outURL.path, contents: nil)
                activeDest = outURL
                activeFile = try FileHandle(forWritingTo: outURL)
                remainingFileBytes = finalSize
                paddingBytes = pad
                if finalSize == 0 {
                    try finishActiveFile()
                }
            } else {
                remainingFileBytes = finalSize
                paddingBytes = pad
            }
        }
    }

    func finish() throws {
        try activeFile?.close()
        activeFile = nil
    }

    private func finishActiveFile() throws {
        try activeFile?.close()
        activeFile = nil
        if var dest = activeDest {
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try? dest.setResourceValues(values)
        }
        activeDest = nil
    }

    private func cString(_ data: Data, offset: Int, length: Int) -> String {
        let slice = data.subdata(in: offset..<(offset + length))
        let trimmed = slice.prefix { $0 != 0 }
        return String(data: trimmed, encoding: .utf8) ?? ""
    }

    private func parseTarSize(_ data: Data, offset: Int, length: Int) -> Int64 {
        let slice = data.subdata(in: offset..<(offset + length))
        if let first = slice.first, (first & 0x80) != 0 {
            var val: UInt64 = UInt64(first & 0x7F)
            for b in slice.dropFirst() {
                val = (val << 8) | UInt64(b)
            }
            return Int64(bitPattern: val)
        }
        let str = cString(data, offset: offset, length: length).trimmingCharacters(in: .whitespacesAndNewlines)
        return Int64(str, radix: 8) ?? 0
    }

    private func parsePax(_ data: Data) {
        guard let text = String(data: data, encoding: .utf8) else { return }
        for line in text.split(separator: "\n") {
            guard let sp = line.firstIndex(of: " "), let eq = line[sp...].firstIndex(of: "=") else { continue }
            let key = String(line[line.index(after: sp)..<eq])
            let val = String(line[line.index(after: eq)...])
            if key == "path" { paxPath = val }
            else if key == "size", let s = Int64(val) { paxSize = s }
        }
    }

    private func sanitizePath(_ raw: String) -> String {
        var comps = raw.split(separator: "/").map(String.init)
        comps.removeAll { $0.isEmpty || $0 == "." }
        if comps.contains("..") { return "" }
        return comps.joined(separator: "/")
    }
}

private final class NxShareModel: NSObject, ObservableObject, URLSessionDownloadDelegate {
    enum Phase {
        case idle
        case fetchingManifest
        case wifiWarning
        case downloading
        case decompressing
        case completed
        case failed(String)
    }

    @Published var phase: Phase = .idle
    @Published var manifest: NxShareManifest?
    @Published var currentFileName: String = ""
    @Published var currentFileIndex: Int = 0
    @Published var totalFiles: Int = 0
    @Published var downloadedBytes: Int64 = 0
    @Published var totalBytes: Int64 = 0
    @Published var decompressedBytes: Int64 = 0
    @Published var uncompressedTotalBytes: Int64 = 0
    @Published var isOnWiFi: Bool = true
    @Published var networkChecked: Bool = false

    let titleId: String
    let appName: String
    private let serverURL: String

    private let monitor = NWPathMonitor()
    private let monitorQueue = DispatchQueue(label: "org.nxnative.nxshare.net")
    private var session: URLSession!
    private var activeTask: URLSessionDownloadTask?
    private var completedBytesBeforeCurrent: Int64 = 0
    private var activeFileDestination: URL?
    private var activeFileExpectedSize: Int64 = 0
    private var activeIsArchive: Bool = false
    private var pendingFiles: [NxShareFile] = []
    private var onComplete: (() -> Void)?

    private static let defaultServerURL = "https://nx.jvrcruz.games"

    override init() {
        let info = Bundle.main.infoDictionary ?? [:]
        self.titleId = (info["DMTitleID"] as? String) ?? ""
        self.appName = (info["CFBundleDisplayName"] as? String) ?? "Game"
        let plistURL = (info["DMNxShareURL"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines)
        self.serverURL = (plistURL?.isEmpty == false ? plistURL! : Self.defaultServerURL).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
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
        fetchManifest()
    }

    func fetchManifest() {
        guard let url = URL(string: "\(serverURL)/api/apps/\(titleId)/manifest") else {
            phase = .failed("Invalid server configuration.")
            return
        }
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
                    self.uncompressedTotalBytes = decoded.uncompressedSize ?? decoded.totalSize
                    if decoded.nxshareVersion == 2, decoded.archive != nil {
                        self.totalFiles = 1
                    } else {
                        self.totalFiles = decoded.files?.count ?? 0
                    }
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
        decompressedBytes = 0
        currentFileIndex = 0

        if manifest.nxshareVersion == 2, let archive = manifest.archive {
            activeIsArchive = true
            pendingFiles = [archive]
        } else {
            activeIsArchive = false
            pendingFiles = manifest.files ?? []
        }
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
        let dest = activeIsArchive
            ? FileManager.default.temporaryDirectory.appendingPathComponent("Documents.archive")
            : documentsDirectory().appendingPathComponent(file.path)
        let fm = FileManager.default

        if !activeIsArchive,
           let attrs = try? fm.attributesOfItem(atPath: dest.path),
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

    private func decompressArchive(at archiveURL: URL) {
        DispatchQueue.main.async {
            self.phase = .decompressing
            self.decompressedBytes = 0
        }
        DispatchQueue.global(qos: .userInitiated).async {
            let fm = FileManager.default
            defer { try? fm.removeItem(at: archiveURL) }
            do {
                let extractor = StreamingTarExtractor(destination: self.documentsDirectory())
                let filter = try OutputFilter(.decompress, using: .lzma) { (data: Data?) in
                    if let data, !data.isEmpty {
                        try extractor.feed(data)
                        let done = extractor.extractedBytes
                        DispatchQueue.main.async {
                            self.decompressedBytes = done
                        }
                    }
                }
                let fh = try FileHandle(forReadingFrom: archiveURL)
                defer { try? fh.close() }
                while true {
                    let chunk = try fh.read(upToCount: 1024 * 1024) ?? Data()
                    if chunk.isEmpty { break }
                    try filter.write(chunk)
                }
                try filter.finalize()
                try extractor.finish()
                DispatchQueue.main.async {
                    self.phase = .completed
                    self.onComplete?()
                }
            } catch {
                DispatchQueue.main.async {
                    self.phase = .failed("Failed to decompress Documents.archive: \(error.localizedDescription)")
                }
            }
        }
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
            if activeIsArchive {
                decompressArchive(at: dest)
                return
            }
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
                    Button("Download game data") {
                        model.fetchManifest()
                    }
                    .disabled(isBusy)
                } header: {
                    Text("Download")
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
                case .decompressing:
                    Section {
                        VStack(alignment: .leading, spacing: 8) {
                            let fraction = model.uncompressedTotalBytes > 0 ? min(1.0, Double(model.decompressedBytes) / Double(model.uncompressedTotalBytes)) : 0.0
                            ProgressView(value: fraction)
                            HStack {
                                Text("Decompressing Documents.archive…")
                                Spacer()
                                Text("\(formatBytes(model.decompressedBytes)) / \(formatBytes(model.uncompressedTotalBytes))")
                                    .monospacedDigit()
                            }
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        }
                        .padding(.vertical, 4)
                    } header: {
                        Text("Extracting")
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
        case .fetchingManifest, .downloading, .decompressing: return true
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
