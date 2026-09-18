#!/usr/bin/env python3
"""Build the QR codes and release notes for one GitHub release.

Every push that produces binaries becomes a release tagged ``R<n>``, holding
every version's ``.cia`` and ``.3dsx`` plus one QR code per version. The QR
encodes nothing but the release asset's download URL, which is what FBI's
Remote Install expects: scan, download, install, no PC involved.

The URL is *predicted* rather than read back from GitHub. A release asset's
address is fixed by the tag and the file name alone --

    https://github.com/<repo>/releases/download/<tag>/<file>

-- so the QR for a release can be generated before that release exists, and is
correct the moment it is published. Nothing here talks to the network.

Encoding choices, both of them about a 2012 camera rather than about QR codes:

* **Error correction M, not L.** The URL is ~79 bytes, which lands in QR
  version 5 (37x37 modules) at either level, so M is free here -- same module
  count, same printed size, more tolerance for a smudged screen.
* **10 pixels per module, 4-module quiet zone**, giving 450x450. The quiet zone
  is not decoration: a QR flush against other content often will not resolve at
  all, and the 3DS camera is 640x480 before it is asked to focus on a monitor.

Requires ``qrencode`` (Debian: ``qrencode``). Standard library otherwise, like
the rest of tools/.

Usage:
    python3 tools/release_assets.py --repo owner/name --tag R47 \\
        --dir dist --notes notes.md
"""

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys

QR_ERROR_CORRECTION = "M"
QR_PIXELS_PER_MODULE = "10"
QR_QUIET_ZONE_MODULES = "4"


def asset_url(repo: str, tag: str, filename: str) -> str:
    return f"https://github.com/{repo}/releases/download/{tag}/{filename}"


def display_name(root: pathlib.Path, version: str) -> str:
    """The manifest's display name, falling back to the bare version id.

    A missing or unreadable manifest is not worth failing a release over --
    the id is already a usable heading.
    """
    manifest = root / "versions" / f"{version}.json"
    try:
        return json.loads(manifest.read_text())["display"]
    except (OSError, ValueError, KeyError):
        return version


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_qr(url: str, out: pathlib.Path) -> None:
    subprocess.run(
        [
            "qrencode",
            "-o", str(out),
            "-s", QR_PIXELS_PER_MODULE,
            "-m", QR_QUIET_ZONE_MODULES,
            "-l", QR_ERROR_CORRECTION,
            url,
        ],
        check=True,
    )


def versions_in(directory: pathlib.Path, tag: str) -> list[tuple[str, pathlib.Path]]:
    """Every (version, cia path) in the directory, by file name.

    Artifacts are named ``3DAlpha<tag><version>.cia``, so stripping the prefix
    leaves the version id. Anything that does not match is ignored rather than
    guessed at.
    """
    prefix = f"3DAlpha{tag}"
    found = []
    for cia in sorted(directory.glob("*.cia")):
        if not cia.stem.startswith(prefix):
            print(f"skipping {cia.name}: not named {prefix}<version>.cia", file=sys.stderr)
            continue
        found.append((cia.stem[len(prefix):], cia))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, help="owner/name")
    parser.add_argument("--tag", required=True, help="release tag, e.g. R47")
    parser.add_argument("--dir", required=True, type=pathlib.Path,
                        help="directory holding the built .cia/.3dsx files")
    parser.add_argument("--notes", required=True, type=pathlib.Path,
                        help="where to write the release body")
    parser.add_argument("--root", default=pathlib.Path("."), type=pathlib.Path,
                        help="repository root, for versions/*.json")
    args = parser.parse_args()

    if shutil.which("qrencode") is None:
        print("qrencode not found (Debian: apt-get install qrencode)", file=sys.stderr)
        return 1

    builds = versions_in(args.dir, args.tag)
    if not builds:
        print(f"no 3DAlpha{args.tag}<version>.cia files in {args.dir}", file=sys.stderr)
        return 1

    lines = [
        f"Build **{args.tag}** of every supported Minecraft version.",
        "",
        "The build number is shared: the same `R` means the same source revision "
        "for every version below, so a version whose code did not change this "
        "push still gets a build here. That is deliberate -- it is what makes "
        "\"am I up to date?\" answerable without knowing which version you run.",
        "",
        "## Installing",
        "",
        "**CIA (HOME menu):** in FBI, *Remote Install -> Scan QR Code*, then scan "
        "the code for your version. The console downloads and installs it on its "
        "own. A newer build installs over the older one rather than beside it.",
        "",
        "**3DSX (Homebrew Launcher):** download the `.3dsx` and drop it in "
        "`sd:/3ds/`. Several versions coexist there by file name.",
        "",
    ]

    for version, cia in builds:
        qr_name = f"qr-{version}.png"
        cia_url = asset_url(args.repo, args.tag, cia.name)
        write_qr(cia_url, args.dir / qr_name)

        lines += [
            f"## {display_name(args.root, version)}",
            "",
            f'<img src="{asset_url(args.repo, args.tag, qr_name)}" width="260" '
            f'alt="FBI install QR for {version}">',
            "",
            f"[`{cia.name}`]({cia_url})",
            "",
            f"`sha256  {sha256(cia)}`",
            "",
        ]

        dsx = cia.with_suffix(".3dsx")
        if dsx.exists():
            lines += [
                f"[`{dsx.name}`]({asset_url(args.repo, args.tag, dsx.name)})",
                "",
                f"`sha256  {sha256(dsx)}`",
                "",
            ]

    args.notes.write_text("\n".join(lines))
    print(f"wrote {args.notes} and {len(builds)} QR code(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
