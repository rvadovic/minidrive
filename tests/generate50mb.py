file_path = "50mb2_file.bin"
size_mb = 250
chunk_size = 1024 * 1024  # 1 MB

with open(file_path, "wb") as f:
    for _ in range(size_mb):
        f.write(b'\0' * chunk_size)

print(f"Created {file_path} ({size_mb} MB)")