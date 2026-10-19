#!/usr/bin/env python3
"""Прогон qrcode1c.dll под wine: стенд harness.exe имитирует 1С, zbarimg распознаёт картинки.

Запуск из корня проекта после ./build.sh:  python3 -I test/run_tests.py
"""
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
OUT_NAME = "Тест кириллица"  # путь с кириллицей и пробелом, как в профиле пользователя
OUT = BUILD / "out" / OUT_NAME

GOST = ("ST0001{enc}|Name=ООО «Ромашка»|PersonalAcc=40702810900000000001|BankName=ПАО СБЕРБАНК"
        "|BIC=044525225|CorrespAcc=30101810400000000225|PayeeINN=7700000000|KPP=770001001"
        "|Sum=150000|Purpose=Оплата по счёту № 15 от 18.10.2026, в т.ч. НДС 20% — 250 руб. ё")

# id, кодировка, уровень, размер модуля, поле, текст, должно получиться, распознавать zbar
CASES = [
    ("ascii", "UTF-8", "M", 4, 4, "Hello, 1C 8.1!", True, True),
    ("gost_utf8", "UTF-8", "M", 4, 4, GOST.format(enc=2), True, True),
    ("gost_1251", "windows-1251", "M", 4, 4, GOST.format(enc=1), True, True),
    ("emoji_utf8", "UTF-8", "Q", 6, 4, "Оплата 😀 ok", True, True),
    ("level_h", "UTF-8", "H", 8, 4, "https://example.com/pay?id=42", True, True),
    ("tiny_no_border", "UTF-8", "M", 1, 0, "tiny", True, False),
    ("max_capacity", "UTF-8", "L", 3, 4, "A" * 2953, True, True),
    ("cyr_1251_fits", "windows-1251", "L", 3, 4, "Ж" * 1500, True, True),
    ("too_long", "UTF-8", "L", 2, 4, "A" * 2954, False, False),
    ("cyr_utf8_too_long", "UTF-8", "L", 2, 4, "Ж" * 1500, False, False),
    ("empty", "UTF-8", "M", 4, 4, "", False, False),
    ("bad_level", "UTF-8", "X", 4, 4, "x", False, False),
    ("bad_scale", "UTF-8", "M", 0, 4, "x", False, False),
    ("huge_image", "UTF-8", "M", 100, 4, "A" * 500, False, False),
]


def check_png(path, scale, border):
    """Разбирает PNG и проверяет структуру; возвращает (ширина, высота)."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "нет сигнатуры PNG"
    pos, chunks = 8, []
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        assert zlib.crc32(kind + body) == crc, f"неверный CRC чанка {kind!r}"
        chunks.append((kind, body))
        pos += 12 + length
    assert [k for k, _ in chunks] == [b"IHDR", b"IDAT", b"IEND"], "неожиданные чанки"
    w, h, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", chunks[0][1])
    # RGB 8 бит на канал: монохромный PNG (1 бит, оттенки серого) 1С 8.1 показывает пустым.
    assert (depth, color, interlace) == (8, 2, 0), "ожидался RGB, 8 бит на канал"
    assert w == h and w % scale == 0 and w // scale >= 21 + 2 * border, f"странный размер {w}x{h}"
    raw = zlib.decompress(chunks[1][1])
    stride = w * 3 + 1
    assert len(raw) == stride * h, "размер распакованных данных не сходится"
    assert all(raw[y * stride] == 0 for y in range(h)), "фильтр строки не 0"
    pixels = b"".join(raw[y * stride + 1:(y + 1) * stride] for y in range(h))
    assert set(pixels) <= {0, 255}, "цвета кроме чёрного и белого"
    assert all(pixels[i] == pixels[i + 1] == pixels[i + 2] for i in range(0, len(pixels), 3)), \
        "пиксель не серый"
    return w, h


def zbar_decode(path):
    res = subprocess.run(["zbarimg", "--raw", "-q", "-Sbinary", str(path)], capture_output=True)
    if res.returncode != 0:
        return None
    return res.stdout[:-1] if res.stdout.endswith(b"\n") else res.stdout


def main():
    for tool in ("wine", "zbarimg"):
        if not shutil.which(tool):
            sys.exit(f"не найден {tool}: sudo apt install wine32 zbar-tools")
    if OUT.parent.exists():
        shutil.rmtree(OUT.parent)
    OUT.mkdir(parents=True)

    lines = [f"{c[0]}\t{c[1]}\t{c[2]}\t{c[3]}\t{c[4]}\t{c[5]}" for c in CASES]
    (BUILD / "cases.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    env = dict(os.environ, WINEDEBUG="-all")
    env.setdefault("WINEPREFIX", str(BUILD / "wineprefix"))
    env.setdefault("WINEARCH", "win32")
    res = subprocess.run(["wine", "harness.exe", "qrcode1c.dll", "cases.txt", f"out\\{OUT_NAME}"],
                         cwd=BUILD, env=env, capture_output=True, timeout=600)
    output = res.stdout.decode("utf-8", "replace")

    failures = 0
    cases = {}
    for line in output.splitlines():
        parts = line.split("\t")
        if parts[0] in ("PASS", "FAIL", "WARN", "INFO"):
            print(f"{parts[0]:5} {parts[1]}")
            failures += parts[0] == "FAIL"
        elif parts[0] == "CASE":
            cases.setdefault(parts[1], {})["status"] = parts[2]
            cases[parts[1]]["error"] = parts[3] if len(parts) > 3 else ""
        elif parts[0] == "CASE_B64":
            cases.setdefault(parts[1], {})["b64"] = parts[2]
        elif parts[0] == "SUMMARY":
            print(f"стенд: {parts[1]}")

    print()
    for cid, enc, level, scale, border, text, expect_ok, decode in CASES:
        got = cases.get(cid)
        problems = []
        if not got:
            problems.append("стенд не выполнил сценарий")
        elif expect_ok:
            if got["status"] != "ok":
                problems.append(f"ожидался успех, ошибка: {got['error']}")
            else:
                if got.get("b64") != "same":
                    problems.append(f"base64 не совпал с файлом ({got.get('b64')})")
                png = OUT / f"{cid}.png"
                try:
                    w, _ = check_png(png, scale, border)
                except AssertionError as e:
                    problems.append(f"PNG: {e}")
                    w = 0
                if decode:
                    expected = text.encode("utf-8" if enc == "UTF-8" else "cp1251")
                    decoded = zbar_decode(png)
                    if decoded is None:
                        problems.append("zbarimg не распознал QR")
                    elif decoded != expected:
                        problems.append(f"распознано не то: {decoded[:60]!r}")
        else:
            if got["status"] != "fail":
                problems.append("ожидалась ошибка, а QR сформирован")
            elif not got["error"]:
                problems.append("пустое ОписаниеОшибки")
            elif got.get("b64") != "empty":
                problems.append("base64-метод вернул непустую строку при ошибке")
        mark = "FAIL " if problems else "PASS "
        detail = "; ".join(problems) if problems else (
            f"{w}px, распознан zbar" if expect_ok and decode else
            f"{w}px" if expect_ok else f"ошибка: {got['error']}")
        print(f"{mark} {cid}: {detail}")
        failures += bool(problems)

    if res.returncode not in (0, 1):
        print(f"FAIL  harness.exe завершился с кодом {res.returncode}")
        print(res.stderr.decode("utf-8", "replace")[-2000:])
        failures += 1
    print(f"\nИтого ошибок: {failures}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
