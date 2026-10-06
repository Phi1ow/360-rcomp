#!/usr/bin/env python3
"""Fetch a pinned, independent FFmpeg codec into a private build directory."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile
import urllib.request

REVISION = "0604b464c7cb4ebc94940cf1f324a3b26b87717c"
SHA256 = "cb05b8b2a0ea051150bd70bc98091cd1e27f92f5000b48bbfe3828b0e89d4991"
URL = f"https://codeload.github.com/wmarti/FFmpeg/tar.gz/{REVISION}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="private directory below this repository's build/")
    args = parser.parse_args()
    build_root = Path(__file__).resolve().parents[1] / "build"
    output = Path(args.output).resolve()
    if not output.is_relative_to(build_root.resolve()):
        parser.error("output must be inside this repository's build directory")
    output.mkdir(parents=True, exist_ok=True)
    archive = output / "source.tar.gz"
    if not archive.exists():
        temporary = output / "source.tar.gz.download"
        with urllib.request.urlopen(URL, timeout=90) as response, temporary.open("wb") as dest:
            while chunk := response.read(1024 * 1024):
                dest.write(chunk)
        temporary.replace(archive)
    with archive.open("rb") as downloaded:
        digest = hashlib.file_digest(downloaded, "sha256").hexdigest()
    if digest != SHA256:
        raise SystemExit("FAIL: codec archive hash mismatch")
    source = output / "source"
    marker = source / ".rcomp-codec-source.json"
    metadata = {"revision": REVISION, "archive_sha256": SHA256, "url": URL,
                "preparation": "omit upstream config.h for private out-of-tree configure"}
    if source.exists():
        if not marker.exists() or json.loads(marker.read_text()) != metadata:
            raise SystemExit("FAIL: existing source lacks the matching preparation marker")
    else:
        source.mkdir()
        with tarfile.open(archive) as bundle:
            members = []
            for member in bundle.getmembers():
                path = PurePosixPath(member.name)
                if not path.parts or path.parts[0] != f"FFmpeg-{REVISION}":
                    raise SystemExit("FAIL: unexpected archive root")
                stripped = PurePosixPath(*path.parts[1:])
                if not path.parts[1:] or str(stripped) == "config.h":
                    continue
                if stripped.is_absolute() or ".." in stripped.parts or member.issym() or member.islnk():
                    raise SystemExit("FAIL: unsafe codec archive member")
                member.name = str(stripped)
                members.append(member)
            bundle.extractall(source, members=members, filter="data")
        marker.write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"PASS verified codec source: {source}")


if __name__ == "__main__":
    main()
