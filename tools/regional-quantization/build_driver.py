"""Build the Windows research driver from a committed ALVR fork checkout."""
from pathlib import Path
import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
DEPENDENCIES = (
    ("ValveSoftware/openvr", "4c85abcb7f7f1f02adaf3812018c99fc593bc341", "openvr"),
    ("alvr-org/settings-schema-rs", "676185f35869f05a90fcc81f66a4bbd9d86f9c12", "vendor/settings-schema"),
    ("Ralith/openxrs", "9270509d23dc774b43a8b7289e8adf69fcac6828", "vendor/openxrs"),
)
MANIFEST_OVERRIDES = (
    ('{ git = "https://github.com/alvr-org/settings-schema-rs", rev = "676185f" }',
     '{ path = "../../vendor/settings-schema/settings-schema" }'),
    ('{ git = "https://github.com/Ralith/openxrs", rev = "9270509d23dc774b43a8b7289e8adf69fcac6828" }',
     '{ path = "../../vendor/openxrs/openxr" }'),
)


def download(url):
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def extract(data, destination, strip_prefix=False):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for member in archive.infolist():
            name = member.filename.partition("/")[2] if strip_prefix else member.filename
            if not name:
                continue
            path = destination / name
            path.resolve().relative_to(destination.resolve())
            if member.is_dir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(archive.read(member))


def clang_dir(cache):
    library = next((cache / "libclang-18.1.1").rglob("libclang.dll"), None)
    if library is None:
        metadata = json.loads(download("https://pypi.org/pypi/libclang/18.1.1/json"))
        wheel = next(item for item in metadata["urls"] if item["filename"].endswith("win_amd64.whl"))
        data = download(wheel["url"])
        if hashlib.sha256(data).hexdigest() != wheel["digests"]["sha256"]:
            raise ValueError("libclang wheel checksum mismatch")
        extract(data, cache / "libclang-18.1.1")
        library = next((cache / "libclang-18.1.1").rglob("libclang.dll"))
    return library.parent


def prepare(cache, source):
    checkout = subprocess.check_output(["git", "archive", "--format=zip", "HEAD"], cwd=ROOT)
    extract(checkout, source)
    archives = cache / "archives"
    archives.mkdir(exist_ok=True)
    for repository, revision, path in DEPENDENCIES:
        archive = archives / f"{repository.rsplit('/', 1)[1]}-{revision}.zip"
        if not archive.is_file():
            archive.write_bytes(download(f"https://codeload.github.com/{repository}/zip/{revision}"))
        extract(archive.read_bytes(), source / path, strip_prefix=True)

    for manifest in (source / "alvr").glob("*/Cargo.toml"):
        original = manifest.read_text(encoding="utf-8")
        updated = original
        for upstream, local in MANIFEST_OVERRIDES:
            updated = updated.replace(upstream, local)
        if updated != original:
            manifest.write_text(updated, encoding="utf-8", newline="\n")
    shutil.copyfile(source / "tools/regional-quantization/Cargo.study.lock", source / "Cargo.lock")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True, help="Build and dependency cache outside the checkout")
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This build targets Windows MSVC")
    cache = args.cache.resolve()
    if cache.is_relative_to(ROOT):
        parser.error("Choose a cache outside the checkout")
    cache.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["LIBCLANG_PATH"] = str(clang_dir(cache))
    environment["CARGO_TARGET_DIR"] = str(cache / "target")
    with tempfile.TemporaryDirectory(dir=cache, prefix="source-") as temporary:
        source = Path(temporary)
        prepare(cache, source)
        subprocess.run(
            ["cargo", "build", "-p", "alvr_server_openvr", "--release", "--locked", "-j", str(args.jobs)],
            cwd=source, env=environment, check=True,
        )
    print(cache / "target/release/alvr_server_openvr.dll")


if __name__ == "__main__":
    main()
