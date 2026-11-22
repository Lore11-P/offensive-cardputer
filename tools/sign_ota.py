#!/usr/bin/env python3
"""Firma il firmware ORBIT OS per l'OTA (ECDSA P-256 / SHA-256).

La chiave privata deve restare FUORI dal repository (default:
~/.orbit-ota/ota_ec_private.pem). Il firmware include solo la chiave pubblica
in include/OtaPublicKey.h.

Uso:
  python tools/sign_ota.py keygen                  # crea chiave + header
  python tools/sign_ota.py sign flasher/bin/firmware.bin
  python tools/sign_ota.py verify flasher/bin/firmware.bin flasher/bin/firmware.sig
  python tools/sign_ota.py selftest

La firma prodotta e' 64 byte grezzi: r (32) || s (32), big-endian.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, utils
except ImportError:  # pragma: no cover
    sys.exit("Serve 'cryptography': python -m pip install cryptography")

DEFAULT_KEY = Path("~/.orbit-ota/ota_ec_private.pem").expanduser()
DEFAULT_HEADER = Path(__file__).resolve().parent.parent / "include" / "OtaPublicKey.h"
SIG_SIZE = 64


def load_private(path: Path) -> ec.EllipticCurvePrivateKey:
    data = path.read_bytes()
    key = serialization.load_pem_private_key(data, password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        sys.exit(f"{path}: serve una chiave ECDSA P-256")
    return key


def raw_public(key: ec.EllipticCurvePublicKey) -> bytes:
    numbers = key.public_numbers()
    return b"\x04" + numbers.x.to_bytes(32, "big") + numbers.y.to_bytes(32, "big")


def write_header(key: ec.EllipticCurvePublicKey, path: Path) -> None:
    pub = raw_public(key)
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "// Generato da tools/sign_ota.py: non modificare a mano.",
        "namespace ota {",
        "constexpr uint8_t kFirmwarePublicKey[65] = {",
    ]
    for start in range(0, len(pub), 12):
        chunk = ", ".join(f"0x{b:02X}" for b in pub[start : start + 12])
        lines.append(f"    {chunk},")
    lines += ["};", "}  // namespace ota", ""]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"header: {path} ({len(pub)} byte di chiave pubblica)")


def signature_bytes(key: ec.EllipticCurvePrivateKey, digest: bytes) -> bytes:
    der = key.sign(digest, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    r, s = utils.decode_dss_signature(der)
    out = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    assert len(out) == SIG_SIZE
    return out


def verify_bytes(key: ec.EllipticCurvePublicKey, digest: bytes, sig: bytes) -> bool:
    if len(sig) != SIG_SIZE:
        return False
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:], "big")
    der = utils.encode_dss_signature(r, s)
    try:
        key.verify(der, digest, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
        return True
    except Exception:
        return False


def cmd_keygen(args: argparse.Namespace) -> None:
    key_path = Path(args.key).expanduser()
    if key_path.exists() and not args.force:
        sys.exit(f"{key_path} esiste gia': non sovrascritta (usa --force)")
    key_path.parent.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    key_path.write_bytes(pem)
    try:
        os.chmod(key_path, 0o600)
    except OSError:
        pass
    write_header(key.public_key(), Path(args.header))
    print(f"chiave privata: {key_path}")
    print("Tienila fuori dal repo e dai backup pubblici; se la perdi, gli")
    print("aggiornamenti OTA richiedono una nuova chiave e un flash USB.")


def cmd_sign(args: argparse.Namespace) -> None:
    bin_path = Path(args.binary)
    if not bin_path.is_file():
        sys.exit(f"{bin_path}: file non trovato")
    digest = hashlib.sha256(bin_path.read_bytes()).digest()
    key = load_private(Path(args.key).expanduser())
    sig = signature_bytes(key, digest)
    out = Path(args.out) if args.out else bin_path.with_suffix(".sig")
    out.write_bytes(sig)
    print(f"firmato: {bin_path} ({bin_path.stat().st_size} byte)")
    print(f"sha256: {digest.hex()}")
    print(f"firma:  {out} ({len(sig)} byte)")


def cmd_manifest(args: argparse.Namespace) -> None:
    bin_path = Path(args.binary)
    if not bin_path.is_file():
        sys.exit(f"{bin_path}: file non trovato")
    version = args.version
    parts = version.split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        sys.exit(f"{version}: serve una versione X.Y.Z")
    data = bin_path.read_bytes()
    text = (
        f"version={version}\n"
        f"size={len(data)}\n"
        f"sha256={hashlib.sha256(data).hexdigest()}\n"
    )
    out = Path(args.out) if args.out else bin_path.with_name("firmware.json")
    out.write_text(text, encoding="ascii")
    print(f"manifest: {out}")
    print(text, end="")


def cmd_verify(args: argparse.Namespace) -> None:
    bin_path = Path(args.binary)
    sig_path = Path(args.sig)
    digest = hashlib.sha256(bin_path.read_bytes()).digest()
    if args.key:
        public = load_private(Path(args.key).expanduser()).public_key()
    else:
        header = Path(args.header).read_text(encoding="utf-8")
        start = header.find("{", header.find("kFirmwarePublicKey"))
        end = header.find("}", start)
        if start < 0 or end < 0:
            sys.exit(f"{args.header}: array della chiave non trovato")
        values = [
            int(token.strip(), 16)
            for token in header[start + 1 : end].split(",")
            if token.strip().startswith("0x")
        ]
        if len(values) < 65 or values[0] != 0x04:
            sys.exit(f"{args.header}: chiave pubblica non trovata")
        public = ec.EllipticCurvePublicKey.from_encoded_point(
            ec.SECP256R1(), bytes(values[:65])
        )
    ok = verify_bytes(public, digest, sig_path.read_bytes())
    print("OK" if ok else "FIRMA NON VALIDA")
    raise SystemExit(0 if ok else 1)


def cmd_selftest(_: argparse.Namespace) -> None:
    key = ec.generate_private_key(ec.SECP256R1())
    digest = hashlib.sha256(b"orbit ota selftest").digest()
    sig = signature_bytes(key, digest)
    assert verify_bytes(key.public_key(), digest, sig)
    assert not verify_bytes(key.public_key(), hashlib.sha256(b"x").digest(), sig)
    print("selftest OK")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("keygen", help="genera chiave privata e header pubblico")
    p.add_argument("--key", default=str(DEFAULT_KEY))
    p.add_argument("--header", default=str(DEFAULT_HEADER))
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_keygen)

    p = sub.add_parser("sign", help="firma firmware.bin -> firmware.sig")
    p.add_argument("binary")
    p.add_argument("--key", default=str(DEFAULT_KEY))
    p.add_argument("--out")
    p.set_defaults(func=cmd_sign)

    p = sub.add_parser("manifest", help="genera firmware.json per l'OTA")
    p.add_argument("binary")
    p.add_argument("--version", required=True)
    p.add_argument("--out")
    p.set_defaults(func=cmd_manifest)

    p = sub.add_parser("verify", help="verifica una firma con la chiave pubblica")
    p.add_argument("binary")
    p.add_argument("sig")
    p.add_argument("--key", help="chiave privata (alternativa all'header)")
    p.add_argument("--header", default=str(DEFAULT_HEADER))
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("selftest", help="round-trip chiave/firma effimera")
    p.set_defaults(func=cmd_selftest)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
