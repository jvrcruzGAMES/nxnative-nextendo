using System;
using System.IO;
using System.IO.Compression;
using System.Collections.Generic;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using Path = System.IO.Path;

static class Program {
    static readonly byte[] BfttfXorKey = new byte[] { 0x49, 0x62, 0x18, 0x06 };

    static int Main(string[] args) {
        if (args.Length < 3) {
            Console.Error.WriteLine("usage: fw_extract <prod.keys> <firmware.zip|dir> <output_dir>");
            return 2;
        }

        string keysPath = args[0];
        string fwPath = args[1];
        string outDir = args[2];

        if (!File.Exists(keysPath)) {
            Console.Error.WriteLine($"[RED] keys file not found: {keysPath}");
            return 1;
        }

        KeySet ks;
        try {
            ks = ExternalKeyReader.ReadKeyFile(keysPath);
        } catch (Exception ex) {
            Console.Error.WriteLine($"[RED] failed to read keys: {ex.Message}");
            return 1;
        }

        Directory.CreateDirectory(outDir);
        string tempExtract = null;

        try {
            string scanDir;
            if (Directory.Exists(fwPath)) {
                scanDir = fwPath;
            } else if (File.Exists(fwPath)) {
                tempExtract = Path.Combine(Path.GetTempPath(), "fw_extract_" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(tempExtract);
                Console.WriteLine($"Unpacking firmware archive {Path.GetFileName(fwPath)}...");
                ZipFile.ExtractToDirectory(fwPath, tempExtract);
                scanDir = tempExtract;
            } else {
                Console.Error.WriteLine($"[RED] firmware path not found: {fwPath}");
                return 1;
            }

            var ncaFiles = Directory.GetFiles(scanDir, "*.nca", SearchOption.AllDirectories);
            Console.WriteLine($"Found {ncaFiles.Length} NCA files in firmware");

            int extractedCount = 0;

            foreach (var ncaPath in ncaFiles) {
                try {
                    using var st = new LocalStorage(ncaPath, FileAccess.Read);
                    var nca = new Nca(ks, st);
                    ulong tid = nca.Header.TitleId;

                    if (nca.Header.ContentType == NcaContentType.Meta) {
                        continue; // Skip CNMT metadata NCAs
                    }

                    if (tid == 0x0100000000000802UL) {
                        // MiiModel / MiiResource
                        if (nca.CanOpenSection(NcaSectionType.Data)) {
                            IFileSystem romfs = nca.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.IgnoreOnInvalid);
                            string dst = Path.Combine(outDir, "sysdata", $"{tid:x16}");
                            Directory.CreateDirectory(dst);
                            CopyFileSystem(romfs, dst);
                            Console.WriteLine($"[firmware] extracted MiiModel ({tid:x16}) to {dst}");
                            extractedCount++;
                        }
                    } else if (tid >= 0x0100000000000810UL && tid <= 0x0100000000000814UL) {
                        // SharedFont
                        if (nca.CanOpenSection(NcaSectionType.Data)) {
                            IFileSystem romfs = nca.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.IgnoreOnInvalid);
                            string dst = Path.Combine(outDir, "sysdata", $"{tid:x16}");
                            Directory.CreateDirectory(dst);
                            CopyFileSystem(romfs, dst);
                            Console.WriteLine($"[firmware] extracted SharedFont ({tid:x16}) to {dst}");

                            string fontDir = Path.Combine(outDir, "font");
                            Directory.CreateDirectory(fontDir);
                            DecryptSharedFonts(dst, fontDir);
                            extractedCount++;
                        }
                    } else if (tid == 0x0100000000001009UL) {
                        // miiEdit applet
                        string appletDir = Path.Combine(outDir, "applets", $"{tid:x16}");
                        Directory.CreateDirectory(appletDir);
                        if (nca.CanOpenSection(NcaSectionType.Data)) {
                            IFileSystem romfs = nca.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.IgnoreOnInvalid);
                            CopyFileSystem(romfs, Path.Combine(appletDir, "RomFS"));
                        }
                        if (nca.CanOpenSection(NcaSectionType.Code)) {
                            IFileSystem exefs = nca.OpenFileSystem(NcaSectionType.Code, IntegrityCheckLevel.IgnoreOnInvalid);
                            CopyFileSystem(exefs, Path.Combine(appletDir, "ExeFS"));
                        }
                        Console.WriteLine($"[firmware] extracted MiiEdit ({tid:x16}) to {appletDir}");
                        extractedCount++;
                    } else if (tid == 0x0100000000000809UL || (tid >= 0x0100000000000800UL && tid <= 0x01000000000008ffUL)) {
                        // System Data Archives (e.g. 081e oceanShared, 0809 TimeZoneBinary, etc.)
                        if (nca.CanOpenSection(NcaSectionType.Data)) {
                            IFileSystem romfs = nca.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.IgnoreOnInvalid);
                            string dst = Path.Combine(outDir, "sysdata", $"{tid:x16}");
                            Directory.CreateDirectory(dst);
                            CopyFileSystem(romfs, dst);
                            Console.WriteLine($"[firmware] extracted SystemData ({tid:x16}) to {dst}");
                            extractedCount++;
                        }
                    }
                } catch {
                    // Skip NCAs that cannot be decrypted or parsed
                }
            }

            Console.WriteLine($"Firmware processing complete. Extracted {extractedCount} components to {outDir}");
            return 0;
        } catch (Exception ex) {
            Console.Error.WriteLine($"[RED] firmware extraction error: {ex}");
            return 1;
        } finally {
            if (tempExtract != null && Directory.Exists(tempExtract)) {
                try { Directory.Delete(tempExtract, true); } catch { }
            }
        }
    }

    static void CopyFileSystem(IFileSystem fs, string dstDir) {
        Directory.CreateDirectory(dstDir);
        foreach (DirectoryEntryEx e in fs.EnumerateEntries("*", SearchOptions.RecurseSubdirectories)) {
            string relPath = e.FullPath.TrimStart('/');
            string dstPath = Path.Combine(dstDir, relPath);
            if (e.Type == DirectoryEntryType.Directory) {
                Directory.CreateDirectory(dstPath);
            } else if (e.Type == DirectoryEntryType.File) {
                Directory.CreateDirectory(Path.GetDirectoryName(dstPath)!);
                using var file = new UniqueRef<IFile>();
                if (fs.OpenFile(ref file.Ref, e.FullPath.ToU8Span(), OpenMode.Read).IsSuccess()) {
                    using var stream = file.Release().AsStream();
                    using var outStream = File.Create(dstPath);
                    stream.CopyTo(outStream);
                }
            }
        }
    }

    static void DecryptSharedFonts(string sysDataDir, string fontDir) {
        if (!Directory.Exists(sysDataDir)) return;
        var files = Directory.GetFiles(sysDataDir, "*", SearchOption.AllDirectories);

        foreach (var srcFile in files) {
            string name = Path.GetFileName(srcFile).ToLowerInvariant();
            string dstName = null;

            if (name.Contains("_std")) {
                dstName = "standard.ttf";
            } else if (name.Contains("zh-cn") && name.Contains("org")) {
                dstName = "chinese_simplified.ttf";
            } else if (name.Contains("zh-cn") && name.Contains("ext")) {
                dstName = "chinese_simplified_ext.ttf";
            } else if (name.Contains("zh-tw")) {
                dstName = "chinese_traditional.ttf";
            } else if (name.Contains("_ko")) {
                dstName = "korean.ttf";
            } else if (name.Contains("_ext2")) {
                dstName = "nintendo_ext2.ttf";
            } else if (name.Contains("_ext")) {
                dstName = "nintendo_ext.ttf";
            }

            if (dstName != null) {
                string dstFile = Path.Combine(fontDir, dstName);
                DecryptBfttf(srcFile, dstFile);
                Console.WriteLine($"[firmware] decrypted font {Path.GetFileName(srcFile)} -> {dstName}");
            }
        }
    }

    static void DecryptBfttf(string srcFile, string dstFile) {
        byte[] src = File.ReadAllBytes(srcFile);
        if (src.Length < 8) return;

        // Check if already TTF or OTF (0x00010000 or OTTO)
        if ((src[0] == 0x00 && src[1] == 0x01 && src[2] == 0x00 && src[3] == 0x00) ||
            (src[0] == 'O' && src[1] == 'T' && src[2] == 'T' && src[3] == 'O')) {
            File.WriteAllBytes(dstFile, src);
            return;
        }

        int offset = 0;
        if ((src[0] ^ BfttfXorKey[0]) == 0x00 && (src[1] ^ BfttfXorKey[1]) == 0x01) {
            offset = 0;
        } else if (src.Length > 8 && (src[8] ^ BfttfXorKey[0]) == 0x00 && (src[9] ^ BfttfXorKey[1]) == 0x01) {
            offset = 8;
        }

        byte[] dst = new byte[src.Length - offset];
        for (int i = 0; i < dst.Length; i++) {
            dst[i] = (byte)(src[offset + i] ^ BfttfXorKey[i % 4]);
        }
        File.WriteAllBytes(dstFile, dst);
    }
}
