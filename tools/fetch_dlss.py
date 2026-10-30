#!/usr/bin/env python3
"""Fetches NVIDIA's DLSS SDK files the engine builds against into external/DLSS (git-ignored).

Frontier F1 T4 (docs/plans/2026-10-30-frontier-f1.md; Frank's ruling F-R3). The SDK is under the
NVIDIA RTX SDKs licence, not MIT, so it is never committed: this script downloads only what the
build needs (the headers, the static NGX loader for the static CRT the engine builds with, the DLSS Ray Reconstruction model and the licence)
(every header: they include each other per platform) from a pinned release, and checks each file against its pinned SHA-256. With them present, CMake
turns OHAO_DLSS on and `--denoise dlss` works on an NVIDIA RTX GPU.

The licence's supplement asks an application that uses DLSS to show the NVIDIA DLSS attribution
(see README.md, "Third-party SDKs") and to notify NVIDIA before a commercial release.

  python tools/fetch_dlss.py                  fetch (skips files already present and correct)
  python tools/fetch_dlss.py --print-hashes   fetch without checking, print the hashes to pin
"""
import argparse
import hashlib
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "external", "DLSS")
TAG = "v310.9.16"
RAW = "https://raw.githubusercontent.com/NVIDIA/DLSS/%s/" % TAG
LFS = "https://media.githubusercontent.com/media/NVIDIA/DLSS/%s/" % TAG

HEADERS = ["nvsdk_ngx.h", "nvsdk_ngx_defs.h", "nvsdk_ngx_defs_dlssd.h", "nvsdk_ngx_defs_dlssg.h",
           "nvsdk_ngx_defs_vk.h", "nvsdk_ngx_helpers.h", "nvsdk_ngx_helpers_cuda.h",
           "nvsdk_ngx_helpers_d3d.h", "nvsdk_ngx_helpers_dlssd.h", "nvsdk_ngx_helpers_dlssd_cuda.h",
           "nvsdk_ngx_helpers_dlssd_d3d.h", "nvsdk_ngx_helpers_dlssd_vk.h", "nvsdk_ngx_helpers_dlssg.h",
           "nvsdk_ngx_helpers_dlssg_d3d.h", "nvsdk_ngx_helpers_dlssg_vk.h", "nvsdk_ngx_helpers_vk.h",
           "nvsdk_ngx_loader.h", "nvsdk_ngx_params.h", "nvsdk_ngx_params_dlssd.h",
           "nvsdk_ngx_params_dlssg.h", "nvsdk_ngx_standalone_common.h", "nvsdk_ngx_standalone_cuda.h",
           "nvsdk_ngx_vk.h"]

# path in the repository -> SHA-256 at TAG
FILES = {
    "LICENSE.txt": "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0",
    "include/nvsdk_ngx.h": "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848",
    "include/nvsdk_ngx_defs.h": "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e",
    "include/nvsdk_ngx_defs_dlssd.h": "2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716",
    "include/nvsdk_ngx_defs_dlssg.h": "c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1",
    "include/nvsdk_ngx_defs_vk.h": "0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031",
    "include/nvsdk_ngx_helpers.h": "5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471",
    "include/nvsdk_ngx_helpers_cuda.h": "f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042",
    "include/nvsdk_ngx_helpers_d3d.h": "ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba",
    "include/nvsdk_ngx_helpers_dlssd.h": "a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532",
    "include/nvsdk_ngx_helpers_dlssd_cuda.h": "7e35cde214d905346a487b9a28c05d35e1c55b480e237fe3de69580a7e332730",
    "include/nvsdk_ngx_helpers_dlssd_d3d.h": "6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293",
    "include/nvsdk_ngx_helpers_dlssd_vk.h": "11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b",
    "include/nvsdk_ngx_helpers_dlssg.h": "6881d21419c40c78ec948b326902ece09fc112b0223144d51c4905f3fced7a1f",
    "include/nvsdk_ngx_helpers_dlssg_d3d.h": "6335ebcb95de39a361e87fe0257d6ce0723e5d8deaa15db3b35afacf2b3f4220",
    "include/nvsdk_ngx_helpers_dlssg_vk.h": "b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316",
    "include/nvsdk_ngx_helpers_vk.h": "c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623",
    "include/nvsdk_ngx_loader.h": "abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75",
    "include/nvsdk_ngx_params.h": "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc",
    "include/nvsdk_ngx_params_dlssd.h": "d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e",
    "include/nvsdk_ngx_params_dlssg.h": "c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad",
    "include/nvsdk_ngx_standalone_common.h": "38413c3338b0aea0202c07da95d426bfee7080a42913823fa2d9dd585b087b53",
    "include/nvsdk_ngx_standalone_cuda.h": "b23436ed5040051ac8e81554c9af080f2463c941435bbc3b82222e5431a09499",
    "include/nvsdk_ngx_vk.h": "2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d",
    "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib": "7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765",
    "lib/Windows_x86_64/rel/nvngx_dlssd.dll": "b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(rel, out):
    os.makedirs(os.path.dirname(out), exist_ok=True)
    for base in (RAW, LFS):
        try:
            with urllib.request.urlopen(base + rel, timeout=120) as r:
                data = r.read()
        except Exception:
            continue
        if data.startswith(b"version https://git-lfs"):   # a pointer: the file is in LFS
            continue
        with open(out, "wb") as f:
            f.write(data)
        return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--print-hashes", action="store_true")
    args = ap.parse_args()
    bad = []
    for rel, pinned in FILES.items():
        out = os.path.join(DEST, rel)
        if not (os.path.exists(out) and pinned and sha256(out) == pinned):
            print("fetch", rel)
            if not download(rel, out):
                bad.append("%s: download failed" % rel)
                continue
        got = sha256(out)
        if args.print_hashes:
            print('    "%s": "%s",' % (rel, got))
        elif got != pinned:
            bad.append("%s: sha256 %s, pinned %s" % (rel, got, pinned or "(none)"))
    if bad:
        print("\n".join(bad), file=sys.stderr)
        return 1
    print("DLSS SDK %s in %s" % (TAG, DEST))
    return 0


if __name__ == "__main__":
    sys.exit(main())
