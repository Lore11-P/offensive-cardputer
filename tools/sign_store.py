#!/usr/bin/env python3
"""Firma il catalogo dello store con la chiave ECDSA P-256 dell'OTA.

Uso:
  python tools/sign_store.py store/games.json
  python tools/sign_store.py store/games.json --out store/games.sig

La firma e' 64 byte grezzi (r||s) sullo SHA-256 del file, come l'OTA.
La chiave privata resta fuori dal repository (~/.orbit-ota/ota_ec_private.pem).
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sign_ota import DEFAULT_KEY, load_private, signature_bytes  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("catalog", nargs="?", default="store/games.json")
    parser.add_argument("--out", default=None)
    parser.add_argument("--key", default=str(DEFAULT_KEY))
    args = parser.parse_args()

    catalog = Path(args.catalog)
    if not catalog.is_file():
        sys.exit(f"catalogo non trovato: {catalog}")
    out = Path(args.out) if args.out else catalog.with_suffix(".sig")
    key_path = Path(args.key).expanduser()
    if not key_path.is_file():
        sys.exit(f"chiave privata non trovata: {key_path}")

    data = catalog.read_bytes()
    digest = hashlib.sha256(data).digest()
    signature = signature_bytes(load_private(key_path), digest)
    out.write_bytes(signature)
    print(f"firmato {catalog} ({len(data)} byte)")
    print(f"sha256 {digest.hex()}")
    print(f"firma  {out} ({len(signature)} byte)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
