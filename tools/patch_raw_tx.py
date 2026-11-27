#!/usr/bin/env python3
"""Abilita deauth/disassoc su IDF 5.4 patchando l'ELF prima di elf2image.

libnet80211.a contiene un controllo chiuso che accetta solo beacon, probe,
action e data non-QoS. Il simbolo e' globale e la funzione e' chiamata
internamente all'archivio: ne' --wrap ne' una definizione forte riescono a
sostituirla. Questo tool trova la funzione nell'ELF e sostituisce il prologo
con `movi.n a2, 0; ret.n` (ritorno 0).

Uso:
    python tools/patch_raw_tx.py firmware/build/orbitos.elf \
        firmware/build/orbitos-patched.elf
    <esptool> elf2image --flash_mode dio --flash_freq 80m --flash_size 8MB \
        --elf-sha256-offset 0xb0 --min-rev-full 0 --max-rev-full 99 \
        -o firmware/build/orbitos-patched.bin firmware/build/orbitos-patched.elf

La patch vale solo per il build corrente: rifarla dopo ogni `idf.py build`.
"""
import struct
import sys


SYMBOL = "ieee80211_raw_frame_sanity_check"
PATCH = bytes([0x0C, 0x02, 0x1D, 0xF0])  # movi.n a2, 0 ; retw.n


def sections(data):
    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize = struct.unpack_from("<H", data, 0x2E)[0]
    shnum = struct.unpack_from("<H", data, 0x30)[0]
    out = []
    for i in range(shnum):
        vals = struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
        out.append(
            {
                "type": vals[1], "addr": vals[3], "offset": vals[4],
                "size": vals[5], "link": vals[6], "entsize": vals[9],
            }
        )
    return out


def find_symbol(data):
    secs = sections(data)
    symtab = next(s for s in secs if s["type"] == 2)  # SHT_SYMTAB
    strings = secs[symtab["link"]]
    strtab = data[strings["offset"]:strings["offset"] + strings["size"]]
    entry = symtab["entsize"] or 16
    for pos in range(symtab["offset"], symtab["offset"] + symtab["size"], entry):
        st_name, st_value, st_size, _info, _other, shndx = struct.unpack_from(
            "<IIIBBH", data, pos
        )
        end = strtab.find(b"\x00", st_name)
        name = strtab[st_name:end].decode("latin1") if st_name else ""
        if name != SYMBOL:
            continue
        sec = secs[shndx]
        if not (sec["addr"] <= st_value < sec["addr"] + sec["size"]):
            raise SystemExit(f"{SYMBOL}: indirizzo fuori sezione")
        return sec["offset"] + (st_value - sec["addr"]), st_value, st_size
    raise SystemExit(f"{SYMBOL} non trovato nell'ELF")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src_path, out_path = sys.argv[1:3]
    elf = bytearray(open(src_path, "rb").read())
    file_off, addr, size = find_symbol(elf)
    if elf[file_off:file_off + 4] == PATCH:
        print("gia' patchato")
    else:
        elf[file_off:file_off + 4] = PATCH
        print(f"patch ok: {SYMBOL} vaddr=0x{addr:08x} size=0x{size:x}")
    open(out_path, "wb").write(elf)
    print(f"ELF patchato -> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
