#!/usr/bin/env python3
"""Build a standard Nintendo Switch RomFS binary image from a directory."""
import os, struct, sys
from pathlib import Path

def romfs_buckets(n):
    if n < 3: return 3
    if n < 19: return n | 1
    while not (n % 2 and n % 3 and n % 5 and n % 7 and n % 11 and n % 13 and n % 17):
        n += 1
    return n

def calc_hash(parent, name_bytes, num_buckets):
    h = (parent ^ 123456789) & 0xffffffff
    for b in name_bytes:
        h = (((h >> 5) | ((h << 27) & 0xffffffff)) ^ b) & 0xffffffff
    return h % num_buckets

class DirNode:
    def __init__(self, name="", parent=0):
        self.name = name
        self.name_bytes = name.encode('utf-8')
        self.parent = parent
        self.child = 0xffffffff
        self.sibling = 0xffffffff
        self.file = 0xffffffff
        self.hash = 0xffffffff
        self.offset = 0
        self.dirs = []
        self.files = []

class FileNode:
    def __init__(self, name="", path=""):
        self.name = name
        self.name_bytes = name.encode('utf-8')
        self.path = path
        self.parent = 0
        self.sibling = 0xffffffff
        self.hash = 0xffffffff
        self.data_offset = 0
        self.data_size = os.path.getsize(path) if path else 0
        self.offset = 0

def scan_dir(root_dir):
    root = DirNode()
    
    def walk(node, current_path):
        entries = sorted(os.listdir(current_path))
        dir_names = [e for e in entries if os.path.isdir(os.path.join(current_path, e))]
        file_names = [e for e in entries if os.path.isfile(os.path.join(current_path, e))]
        
        for d in dir_names:
            child_node = DirNode(d)
            node.dirs.append(child_node)
            walk(child_node, os.path.join(current_path, d))
            
        for f in file_names:
            file_node = FileNode(f, os.path.join(current_path, f))
            node.files.append(file_node)
            
    walk(root, root_dir)
    return root

def build_romfs(root_dir, output_file):
    root = scan_dir(root_dir)
    
    # Linearize dirs and files
    all_dirs = []
    all_files = []
    
    def collect(d):
        all_dirs.append(d)
        for sub in d.dirs:
            collect(sub)
        for f in d.files:
            all_files.append(f)
            
    collect(root)
    
    # Calculate offsets in dir_meta_table and file_meta_table
    dir_offset = 0
    for d in all_dirs:
        d.offset = dir_offset
        # 24 bytes header + name + padding
        entry_size = (24 + len(d.name_bytes) + 3) & ~3
        dir_offset += entry_size
    dir_meta_size = dir_offset
    
    file_offset = 0
    for f in all_files:
        f.offset = file_offset
        # 32 bytes header + name + padding
        entry_size = (32 + len(f.name_bytes) + 3) & ~3
        file_offset += entry_size
    file_meta_size = file_offset
    
    # Setup parent/sibling/child links
    for d in all_dirs:
        if d.dirs:
            d.child = d.dirs[0].offset
            for i in range(len(d.dirs) - 1):
                d.dirs[i].sibling = d.dirs[i + 1].offset
            for sub in d.dirs:
                sub.parent = d.offset
        if d.files:
            d.file = d.files[0].offset
            for i in range(len(d.files) - 1):
                d.files[i].sibling = d.files[i + 1].offset
            for f in d.files:
                f.parent = d.offset

    num_dir_buckets = romfs_buckets(len(all_dirs))
    num_file_buckets = romfs_buckets(len(all_files)) if all_files else 3
    
    dir_hash_table = [0xffffffff] * num_dir_buckets
    file_hash_table = [0xffffffff] * num_file_buckets
    
    # Populate dir hash table
    for d in all_dirs:
        b = calc_hash(d.parent, d.name_bytes, num_dir_buckets)
        d.hash = dir_hash_table[b]
        dir_hash_table[b] = d.offset
        
    # Populate file hash table
    for f in all_files:
        b = calc_hash(f.parent, f.name_bytes, num_file_buckets)
        f.hash = file_hash_table[b]
        file_hash_table[b] = f.offset
        
    # Layout sections
    header_size = 0x50
    dir_hash_table_offset = header_size
    dir_hash_table_size = 4 * num_dir_buckets
    
    dir_meta_table_offset = (dir_hash_table_offset + dir_hash_table_size + 3) & ~3
    # Update size if alignment added padding
    dir_hash_table_size = dir_meta_table_offset - dir_hash_table_offset
    
    file_hash_table_offset = (dir_meta_table_offset + dir_meta_size + 3) & ~3
    dir_meta_table_size = file_hash_table_offset - dir_meta_table_offset
    
    file_hash_table_size = 4 * num_file_buckets
    file_meta_table_offset = (file_hash_table_offset + file_hash_table_size + 3) & ~3
    file_hash_table_size = file_meta_table_offset - file_hash_table_offset
    
    data_offset = (file_meta_table_offset + file_meta_size + 0x1ff) & ~0x1ff
    file_meta_table_size = data_offset - file_meta_table_offset
    
    # Align each file's data_offset to 0x10
    cur_data_off = 0
    for f in all_files:
        cur_data_off = (cur_data_off + 0xf) & ~0xf
        f.data_offset = cur_data_off
        cur_data_off += f.data_size
        
    total_size = data_offset + cur_data_off
    
    print(f"Building RomFS: {len(all_dirs)} dirs, {len(all_files)} files, data_offset=0x{data_offset:x}, total=0x{total_size:x} ({total_size/(1024*1024):.2f} MB)")
    
    with open(output_file, 'wb') as out:
        # Write header (0x50 bytes: 10 x uint64)
        hdr = struct.pack('<10Q',
            header_size,
            dir_hash_table_offset, dir_hash_table_size,
            dir_meta_table_offset, dir_meta_table_size,
            file_hash_table_offset, file_hash_table_size,
            file_meta_table_offset, file_meta_table_size,
            data_offset
        )
        out.write(hdr)
        
        # Write dir hash table
        for b in dir_hash_table:
            out.write(struct.pack('<I', b))
        if out.tell() < dir_meta_table_offset:
            out.write(b'\0' * (dir_meta_table_offset - out.tell()))
            
        # Write dir meta table
        for d in all_dirs:
            # uint32 parent, sibling, child, file, hash, name_size
            d_hdr = struct.pack('<6I', d.parent, d.sibling, d.child, d.file, d.hash, len(d.name_bytes))
            out.write(d_hdr)
            out.write(d.name_bytes)
            entry_sz = 24 + len(d.name_bytes)
            pad = ((entry_sz + 3) & ~3) - entry_sz
            if pad: out.write(b'\0' * pad)
        if out.tell() < file_hash_table_offset:
            out.write(b'\0' * (file_hash_table_offset - out.tell()))
            
        # Write file hash table
        for b in file_hash_table:
            out.write(struct.pack('<I', b))
        if out.tell() < file_meta_table_offset:
            out.write(b'\0' * (file_meta_table_offset - out.tell()))
            
        # Write file meta table
        for f in all_files:
            # uint32 parent, sibling, uint64 offset, size, uint32 hash, name_size
            f_hdr = struct.pack('<2I2Q2I', f.parent, f.sibling, f.data_offset, f.data_size, f.hash, len(f.name_bytes))
            out.write(f_hdr)
            out.write(f.name_bytes)
            entry_sz = 32 + len(f.name_bytes)
            pad = ((entry_sz + 3) & ~3) - entry_sz
            if pad: out.write(b'\0' * pad)
        if out.tell() < data_offset:
            out.write(b'\0' * (data_offset - out.tell()))
            
        # Write file data
        cur_written = 0
        for f in all_files:
            target_off = f.data_offset
            if cur_written < target_off:
                out.write(b'\0' * (target_off - cur_written))
                cur_written = target_off
            with open(f.path, 'rb') as src:
                while True:
                    chunk = src.read(1024 * 1024)
                    if not chunk: break
                    out.write(chunk)
                    cur_written += len(chunk)

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <romfs_dir> <output_romfs.bin>")
        sys.exit(1)
    build_romfs(sys.argv[1], sys.argv[2])
