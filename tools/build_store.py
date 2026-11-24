#!/usr/bin/env python3
"""Genera store/games.json dai file store/games/*.wren.

Metadati letti dalle prime righe di ogni gioco:
    // name: Snake
    // desc: Mangia e cresci senza morderti la coda.
    // version: 1.0        (opzionale, default 1.0)
"""

import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAMES_DIR = ROOT / "store" / "games"
OUTPUT = ROOT / "store" / "games.json"
BASE_URL = "https://raw.githubusercontent.com/Lore11-P/offensive-cardputer/main/store/games/"
MAX_GAME_BYTES = 32768
MAX_CATALOG_BYTES = 32768
MAX_GAMES = 24
ID_RE = re.compile(r"^[a-z0-9_-]+$")
META_RE = re.compile(r"^//\s*(name|desc|version)\s*:\s*(.+?)\s*$")


def metadata(text, fallback_name):
    name = fallback_name
    desc = ""
    version = "1.0"
    for line in text.splitlines()[:8]:
        match = META_RE.match(line)
        if match is None:
            continue
        key, value = match.group(1), match.group(2)
        if key == "name":
            name = value
        elif key == "desc":
            desc = value
        else:
            version = value
    return name, desc, version


def main():
    if not GAMES_DIR.is_dir():
        sys.exit(f"cartella non trovata: {GAMES_DIR}")

    games = []
    for path in sorted(GAMES_DIR.glob("*.wren")):
        game_id = path.stem
        if len(game_id) > 24 or not ID_RE.match(game_id):
            sys.exit(f"id non valido: {path.name} (1-24 caratteri [a-z0-9_-])")

        data = path.read_bytes()
        # GitHub salva i file con LF: normalizza per non firmare gli hash di
        # un checkout Windows con core.autocrlf=true.
        data = data.replace(b"\r\n", b"\n")
        if len(data) > MAX_GAME_BYTES:
            sys.exit(f"{path.name}: {len(data)} byte, limite {MAX_GAME_BYTES}")

        fallback = game_id.replace("-", " ").replace("_", " ").title()
        name, desc, version = metadata(data.decode("utf-8"), fallback)
        if len(name) > 24 or len(desc) > 64 or len(version) > 8:
            sys.exit(f"{path.name}: name<=24, desc<=64, version<=8")
        games.append(
            {
                "id": game_id,
                "name": name,
                "version": version,
                "desc": desc,
                "url": BASE_URL + path.name,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )

    games.sort(key=lambda game: game["name"])
    if len(games) > MAX_GAMES:
        sys.exit(f"troppi giochi: {len(games)} (max {MAX_GAMES})")
    catalog = {"version": 1, "games": games}

    payload = json.dumps(catalog, ensure_ascii=False, indent=2) + "\n"
    encoded = payload.encode("utf-8")
    if len(encoded) > MAX_CATALOG_BYTES:
        sys.exit(f"catalogo {len(encoded)} byte, limite {MAX_CATALOG_BYTES}")
    with OUTPUT.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(payload)

    for game in games:
        print(f"{game['id']:12} {game['size']:6} B  {game['sha256'][:12]}")
    print(f"{OUTPUT.relative_to(ROOT)}: {len(games)} giochi")
    if OUTPUT.with_suffix(".sig").exists():
        print("ATTENZIONE: catalogo modificato; rifirma con tools/sign_store.py")


if __name__ == "__main__":
    main()
