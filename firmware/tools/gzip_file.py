"""gzip a file reproducibly (mtime 0) - used by main/CMakeLists.txt to embed the web UI."""
import gzip
import sys

with open(sys.argv[1], "rb") as src:
    data = src.read()
with open(sys.argv[2], "wb") as dst:
    dst.write(gzip.compress(data, compresslevel=9, mtime=0))
