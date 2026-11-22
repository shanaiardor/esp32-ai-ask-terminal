#!/usr/bin/env python3
"""
Generate LVGL v8 fonts for ai_ask_terminal:
  - main/fonts/font_clock_64.c : Bahnschrift 64px tabular digits "0123456789:" (4bpp)
  - main/fonts/font_clock_32.c : Bahnschrift 32px tabular digits "0123456789:.-/ %V+-smh" (4bpp)
  - fonts_bin/font_cn_16.bin   : HarmonyOS Sans SC Medium 16px ASCII + FULL 20,902 CJK Hanzi
                                 (U+4E00..U+9FA5) + punctuation & symbols (4bpp high quality!)
                                 Flashed to independent `font_dat` partition & mmap'd at boot.
"""

import os
import struct
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT_C_DIR = os.path.join(ROOT_DIR, "main", "fonts")
OUT_BIN_DIR = os.path.join(ROOT_DIR, "fonts_bin")
FONT_HARMONYOS_SANS = os.path.join(ROOT_DIR, "tools", "fonts", "HarmonyOS_Sans_SC_Medium.ttf")
FONT_BAHNSCHRIFT = r"C:\Windows\Fonts\bahnschrift.ttf"
FONT_SEGUI_SYM = r"C:\Windows\Fonts\seguisym.ttf"

# Common UI/tech/status symbols that HarmonyOS Sans SC may not include;
# rendered via Segoe UI Symbol fallback when missing from primary font.
FALLBACK_EXTRA_SYMBOLS = (
    "✓✔✕✖✗✘⚠⚡⚙☐☑☒"
    "➔➜➤➡⬅⬆⬇↗↘↙↖↩↪↻↺"
    "☀☁☂❄☎✉✎✏✂✈♠♣♥♦♪♫♬"
    "⏳⌛⏰⏱ℹ⭕❌❓❗⭐"
)

FONT_BIN_MAGIC = 0x464E5431  # 'FNT1'
CMAP_FORMAT0_TINY = 2
CMAP_SPARSE_TINY = 3


def read_ttf_bmp_codepoints(ttf_path: str) -> set[int]:
    """Read all BMP (U+0020..U+FFFF) codepoints present in a TrueType font's cmap table."""
    with open(ttf_path, "rb") as f:
        data = f.read()
    num_tables = struct.unpack(">H", data[4:6])[0]
    cmap_off = None
    for i in range(num_tables):
        tag, _, off, _ = struct.unpack(">4sIII", data[12 + i * 16 : 28 + i * 16])
        if tag == b"cmap":
            cmap_off = off
            break
    if cmap_off is None:
        return set()
    num_sub = struct.unpack(">H", data[cmap_off + 2 : cmap_off + 4])[0]
    cps: set[int] = set()
    for i in range(num_sub):
        _, _, sub_off = struct.unpack(">HHI", data[cmap_off + 4 + i * 8 : cmap_off + 12 + i * 8])
        o = cmap_off + sub_off
        fmt = struct.unpack(">H", data[o : o + 2])[0]
        if fmt == 4:
            seg_cnt = struct.unpack(">H", data[o + 6 : o + 8])[0] // 2
            ends = struct.unpack(f">{seg_cnt}H", data[o + 14 : o + 14 + seg_cnt * 2])
            starts = struct.unpack(f">{seg_cnt}H", data[o + 16 + seg_cnt * 2 : o + 16 + seg_cnt * 4])
            for s, e in zip(starts, ends):
                if s == 0xFFFF:
                    break
                for cp in range(max(0x20, s), min(0x10000, e + 1)):
                    cps.add(cp)
        elif fmt == 12:
            ng = struct.unpack(">I", data[o + 12 : o + 16])[0]
            for g in range(ng):
                s, e, _ = struct.unpack(">III", data[o + 16 + g * 12 : o + 28 + g * 12])
                if s >= 0x10000:
                    continue
                for cp in range(max(0x20, s), min(0x10000, e + 1)):
                    cps.add(cp)
    # Exclude private-use area (U+E000..U+F8FF) and surrogates (U+D800..U+DFFF)
    return {cp for cp in cps if not (0xD800 <= cp <= 0xF8FF) and cp != 0xFFFF and cp != 0xFEFF}


def load_font(path: str, size: int, variation_wght: int | None = None) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(path, size=size)
    if variation_wght is not None:
        try:
            axes = font.get_variation_axes()
            vals = []
            for ax in axes:
                name = ax.get("name", b"")
                if isinstance(name, bytes):
                    name = name.decode("ascii", errors="ignore")
                if "weight" in name.lower():
                    vals.append(max(ax["minimum"], min(ax["maximum"], variation_wght)))
                else:
                    vals.append(ax["default"])
            if vals:
                font.set_variation_by_axes(vals)
        except Exception:
            pass
    return font


def pack_bpp(gray_img: Image.Image, bpp: int) -> bytes:
    if hasattr(gray_img, "get_flattened_data"):
        pixels = list(gray_img.get_flattened_data())
    else:
        pixels = list(gray_img.getdata())

    if bpp == 4:
        nibbles = [(p * 15 + 127) // 255 for p in pixels]
        out = bytearray()
        for i in range(0, len(nibbles), 2):
            hi = nibbles[i] & 0x0F
            lo = (nibbles[i + 1] & 0x0F) if (i + 1 < len(nibbles)) else 0
            out.append((hi << 4) | lo)
        return bytes(out)

    if bpp == 2:
        crumbs = [(p * 3 + 127) // 255 for p in pixels]
        out = bytearray()
        for i in range(0, len(crumbs), 4):
            b = 0
            for k in range(4):
                v = (crumbs[i + k] & 0x03) if (i + k < len(crumbs)) else 0
                b |= v << (6 - k * 2)
            out.append(b)
        return bytes(out)

    raise ValueError(f"Unsupported bpp: {bpp}")


def render_glyph(font: ImageFont.FreeTypeFont, ch: str, bpp: int):
    adv_px = font.getlength(ch)
    adv_w = int(round(adv_px * 16))
    if ch == " ":
        return b"", 0, 0, 0, 0, adv_w

    bbox = font.getbbox(ch, anchor="ls")
    if not bbox:
        return b"", 0, 0, 0, 0, adv_w
    left, top, right, bottom = bbox
    box_w = max(0, right - left)
    box_h = max(0, bottom - top)
    if box_w == 0 or box_h == 0:
        return b"", 0, 0, 0, 0, adv_w

    img = Image.new("L", (box_w, box_h), 0)
    draw = ImageDraw.Draw(img)
    draw.text((-left, -top), ch, font=font, fill=255, anchor="ls")
    bmp = pack_bpp(img, bpp)
    ofs_x = left
    ofs_y = -bottom
    return bmp, box_w, box_h, ofs_x, ofs_y, adv_w


def prepare_font_data(
    font: ImageFont.FreeTypeFont,
    chars: list[str],
    bpp: int,
    tabular_digits: bool = False,
    tight_line_height: bool = False,
    primary_cps: set[int] | None = None,
    fallback_font: ImageFont.FreeTypeFont | None = None,
):
    codepoints = sorted({ord(c) for c in chars})
    glyphs = []
    for cp in codepoints:
        ch = chr(cp)
        use_font = font
        if primary_cps is not None and cp not in primary_cps and fallback_font is not None:
            use_font = fallback_font
        bmp, bw, bh, ox, oy, aw = render_glyph(use_font, ch, bpp)
        glyphs.append(
            {
                "cp": cp,
                "ch": ch,
                "bmp": bmp,
                "box_w": bw,
                "box_h": bh,
                "ofs_x": ox,
                "ofs_y": oy,
                "adv_w": aw,
            }
        )

    if tabular_digits:
        digit_glyphs = [g for g in glyphs if ord("0") <= g["cp"] <= ord("9")]
        if digit_glyphs:
            max_adv = max(g["adv_w"] for g in digit_glyphs)
            max_box_pad = max((g["box_w"] + 2) * 16 for g in digit_glyphs)
            target_adv = max(max_adv, max_box_pad)
            for g in digit_glyphs:
                adv_px = target_adv / 16.0
                g["ofs_x"] = int(round((adv_px - g["box_w"]) / 2.0))
                g["adv_w"] = target_adv

    non_empty = [g for g in glyphs if g["box_h"] > 0]
    max_ascent = max((g["box_h"] + g["ofs_y"]) for g in non_empty) if non_empty else 16
    max_descent = max((-g["ofs_y"]) for g in non_empty) if non_empty else 0
    max_descent = max(0, max_descent)
    if tight_line_height:
        line_height = max_ascent + max_descent
        base_line = max_descent
    else:
        ascent_f, descent_f = font.getmetrics()
        # Use primary font's native vertical metrics so tall outlier symbols
        # (e.g. box drawing / integrals) don't inflate line_height > 22px and
        # trigger unwanted vertical marquee scrolling on single-line labels.
        line_height = ascent_f + descent_f
        base_line = descent_f

    bitmap_bytes = bytearray()
    for g in glyphs:
        g["bitmap_index"] = len(bitmap_bytes)
        bitmap_bytes.extend(g["bmp"])

    cmaps = []
    idx_map = {g["cp"]: i + 1 for i, g in enumerate(glyphs)}
    i = 0
    n = len(glyphs)
    sparse_buf = []

    def flush_sparse():
        nonlocal sparse_buf
        if not sparse_buf:
            return
        chunk = []
        for g in sparse_buf:
            if not chunk or (g["cp"] - chunk[0]["cp"] + 1) <= 65000:
                chunk.append(g)
            else:
                rs = chunk[0]["cp"]
                cmaps.append(
                    {
                        "range_start": rs,
                        "range_length": chunk[-1]["cp"] - rs + 1,
                        "glyph_id_start": idx_map[rs],
                        "unicode_list": [x["cp"] - rs for x in chunk],
                        "type_id": CMAP_SPARSE_TINY,
                        "type": "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY",
                    }
                )
                chunk = [g]
        if chunk:
            rs = chunk[0]["cp"]
            cmaps.append(
                {
                    "range_start": rs,
                    "range_length": chunk[-1]["cp"] - rs + 1,
                    "glyph_id_start": idx_map[rs],
                    "unicode_list": [x["cp"] - rs for x in chunk],
                    "type_id": CMAP_SPARSE_TINY,
                    "type": "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY",
                }
            )
        sparse_buf = []

    while i < n:
        j = i
        while j + 1 < n and glyphs[j + 1]["cp"] == glyphs[j]["cp"] + 1:
            j += 1
        run_len = j - i + 1
        if run_len >= 64:
            flush_sparse()
            rs = glyphs[i]["cp"]
            cmaps.append(
                {
                    "range_start": rs,
                    "range_length": run_len,
                    "glyph_id_start": idx_map[rs],
                    "unicode_list": None,
                    "type_id": CMAP_FORMAT0_TINY,
                    "type": "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY",
                }
            )
            i = j + 1
        else:
            sparse_buf.extend(glyphs[i : j + 1])
            i = j + 1
    flush_sparse()

    return glyphs, bytes(bitmap_bytes), cmaps, line_height, base_line


def build_font_c(
    out_path: str,
    sym_name: str,
    font: ImageFont.FreeTypeFont,
    chars: list[str],
    bpp: int = 4,
    tabular_digits: bool = False,
    fallback_sym: str | None = None,
    tight_line_height: bool = False,
):
    glyphs, bitmap_bytes, cmaps, line_height, base_line = prepare_font_data(
        font, chars, bpp, tabular_digits=tabular_digits, tight_line_height=tight_line_height
    )

    lines = []
    lines.append("/* Auto-generated by tools/gen_fonts.py - DO NOT EDIT */")
    lines.append('#include "lvgl.h"')
    lines.append("")
    if fallback_sym:
        lines.append(f"extern const lv_font_t {fallback_sym};")
        lines.append("")

    lines.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    if len(bitmap_bytes) == 0:
        lines.append("    0x00")
    else:
        for idx in range(0, len(bitmap_bytes), 24):
            row = ",".join(f"0x{b:02x}" for b in bitmap_bytes[idx : idx + 24])
            comma = "," if (idx + 24 < len(bitmap_bytes)) else ""
            lines.append(f"    {row}{comma}")
    lines.append("};")
    lines.append("")

    lines.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    lines.append("    {0,0,0,0,0,0},")
    for idx, g in enumerate(glyphs):
        comma = "," if idx < len(glyphs) - 1 else ""
        lines.append(
            f"    {{{g['bitmap_index']},{g['adv_w']},{g['box_w']},{g['box_h']},{g['ofs_x']},{g['ofs_y']}}}{comma}"
        )
    lines.append("};")
    lines.append("")

    for ci, cm in enumerate(cmaps):
        if cm["unicode_list"] is not None:
            lines.append(f"static const uint16_t unicode_list_{ci}[] = {{")
            ulist = cm["unicode_list"]
            for idx in range(0, len(ulist), 16):
                row = ",".join(f"0x{v:04x}" for v in ulist[idx : idx + 16])
                comma = "," if (idx + 16 < len(ulist)) else ""
                lines.append(f"    {row}{comma}")
            lines.append("};")
            lines.append("")

    lines.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    for ci, cm in enumerate(cmaps):
        ul_ptr = f"unicode_list_{ci}" if cm["unicode_list"] is not None else "NULL"
        llen = len(cm["unicode_list"]) if cm["unicode_list"] is not None else 0
        comma = "," if ci < len(cmaps) - 1 else ""
        lines.append("    {")
        lines.append(f"        .range_start = {cm['range_start']}, .range_length = {cm['range_length']}, .glyph_id_start = {cm['glyph_id_start']},")
        lines.append(f"        .unicode_list = {ul_ptr}, .glyph_id_ofs_list = NULL, .list_length = {llen}, .type = {cm['type']}")
        lines.append(f"    }}{comma}")
    lines.append("};")
    lines.append("")

    lines.append("static lv_font_fmt_txt_glyph_cache_t cache;")
    lines.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    lines.append("    .glyph_bitmap = glyph_bitmap,")
    lines.append("    .glyph_dsc = glyph_dsc,")
    lines.append("    .cmaps = cmaps,")
    lines.append("    .kern_dsc = NULL,")
    lines.append("    .kern_scale = 0,")
    lines.append(f"    .cmap_num = {len(cmaps)},")
    lines.append(f"    .bpp = {bpp},")
    lines.append("    .kern_classes = 0,")
    lines.append("    .bitmap_format = 0,")
    lines.append("    .cache = &cache")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_font_t {sym_name} = {{")
    lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    lines.append(f"    .line_height = {line_height},")
    lines.append(f"    .base_line = {base_line},")
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append("    .underline_position = -2,")
    lines.append("    .underline_thickness = 1,")
    lines.append("    .dsc = &font_dsc,")
    lines.append(f"    .fallback = {'&' + fallback_sym if fallback_sym else 'NULL'}")
    lines.append("};")
    lines.append("")

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print(f"Generated {os.path.basename(out_path)}: glyphs={len(glyphs)}, bpp={bpp}, bitmap={len(bitmap_bytes)} bytes")


def build_font_bin(
    out_path: str,
    font: ImageFont.FreeTypeFont,
    chars: list[str],
    bpp: int = 4,
    primary_cps: set[int] | None = None,
    fallback_font: ImageFont.FreeTypeFont | None = None,
):
    glyphs, bitmap_bytes, cmaps, line_height, base_line = prepare_font_data(
        font, chars, bpp, tabular_digits=False, tight_line_height=False,
        primary_cps=primary_cps, fallback_font=fallback_font,
    )

    # Binary layout (all 4-byte aligned, little-endian for ESP32-S3 mmap zero-copy):
    # Header (32B):
    #   uint32 magic, uint16 line_height, uint16 base_line,
    #   uint8 bpp, uint8 cmap_num, uint16 reserved,
    #   uint32 glyph_count, uint32 glyph_dsc_ofs,
    #   uint32 cmaps_ofs, uint32 bitmap_ofs, uint32 total_size
    header_size = 32
    glyph_dsc_ofs = header_size
    # lv_font_fmt_txt_glyph_dsc_t (with LV_FONT_FMT_TXT_LARGE=1):
    #   uint32 bitmap_index, uint32 adv_w, uint16 box_w, uint16 box_h, int16 ofs_x, int16 ofs_y (16B)
    dsc_buf = bytearray()
    dsc_buf.extend(struct.pack("<IIHHhh", 0, 0, 0, 0, 0, 0))  # glyph 0 (reserved)
    for g in glyphs:
        dsc_buf.extend(struct.pack("<IIHHhh", g["bitmap_index"], g["adv_w"], g["box_w"], g["box_h"], g["ofs_x"], g["ofs_y"]))

    cmaps_ofs = glyph_dsc_ofs + len(dsc_buf)
    # Each bin_cmap_t is 16 bytes:
    #   uint32 range_start, uint16 range_length, uint16 glyph_id_start,
    #   uint16 list_length, uint16 type_id, uint32 unicode_list_ofs
    cmap_table_size = len(cmaps) * 16
    ulist_cursor = cmaps_ofs + cmap_table_size
    cmaps_buf = bytearray()
    ulists_buf = bytearray()

    for cm in cmaps:
        if cm["unicode_list"] is not None:
            ul_ofs = ulist_cursor
            raw_ul = struct.pack(f"<{len(cm['unicode_list'])}H", *cm["unicode_list"])
            pad = (4 - (len(raw_ul) % 4)) % 4
            ulists_buf.extend(raw_ul)
            ulists_buf.extend(b"\x00" * pad)
            ulist_cursor += len(raw_ul) + pad
            llen = len(cm["unicode_list"])
        else:
            ul_ofs = 0
            llen = 0
        cmaps_buf.extend(struct.pack("<IHHHHI", cm["range_start"], cm["range_length"], cm["glyph_id_start"], llen, cm["type_id"], ul_ofs))

    bitmap_ofs = ulist_cursor
    bmp_pad = (4 - (len(bitmap_bytes) % 4)) % 4
    total_size = bitmap_ofs + len(bitmap_bytes) + bmp_pad

    hdr = struct.pack(
        "<IHHBBHIIIII",
        FONT_BIN_MAGIC,
        line_height,
        base_line,
        bpp,
        len(cmaps),
        0,
        len(glyphs) + 1,
        glyph_dsc_ofs,
        cmaps_ofs,
        bitmap_ofs,
        total_size,
    )

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(hdr)
        f.write(dsc_buf)
        f.write(cmaps_buf)
        f.write(ulists_buf)
        f.write(bitmap_bytes)
        f.write(b"\x00" * bmp_pad)

    print(
        f"Generated binary {os.path.basename(out_path)}: glyphs={len(glyphs)+1}, bpp={bpp}, "
        f"cmaps={len(cmaps)}, total_size={total_size} bytes ({total_size/1024/1024:.2f} MB)"
    )


def main():
    os.makedirs(OUT_C_DIR, exist_ok=True)
    os.makedirs(OUT_BIN_DIR, exist_ok=True)
    cn_font_path = FONT_HARMONYOS_SANS
    if not os.path.exists(cn_font_path):
        print(f"ERROR: Missing HarmonyOS Sans SC font {cn_font_path}", file=sys.stderr)
        sys.exit(1)

    digits64 = list("0123456789:")
    small_digits = list("0123456789:.-/ %V+-smh")

    primary_cps = read_ttf_bmp_codepoints(cn_font_path)
    sym_font = None
    sym_cps: set[int] = set()
    if os.path.exists(FONT_SEGUI_SYM):
        sym_font = load_font(FONT_SEGUI_SYM, 15)
        sym_cps = read_ttf_bmp_codepoints(FONT_SEGUI_SYM)

    all_cps = set(primary_cps)
    for ch in FALLBACK_EXTRA_SYMBOLS:
        cp = ord(ch)
        if cp in primary_cps or cp in sym_cps:
            all_cps.add(cp)
    cn_chars = [chr(cp) for cp in sorted(all_cps)]

    num_font_path = FONT_BAHNSCHRIFT if os.path.exists(FONT_BAHNSCHRIFT) else cn_font_path
    f64 = load_font(num_font_path, 64, variation_wght=600)
    f32 = load_font(num_font_path, 32, variation_wght=600)
    f16 = load_font(cn_font_path, 16)

    build_font_c(os.path.join(OUT_C_DIR, "font_clock_64.c"), "font_clock_64", f64, digits64, bpp=4, tabular_digits=True, tight_line_height=True)
    build_font_c(os.path.join(OUT_C_DIR, "font_clock_32.c"), "font_clock_32", f32, small_digits, bpp=4, tabular_digits=True, tight_line_height=True)
    build_font_bin(
        os.path.join(OUT_BIN_DIR, "font_cn_16.bin"),
        f16,
        cn_chars,
        bpp=4,
        primary_cps=primary_cps,
        fallback_font=sym_font,
    )


if __name__ == "__main__":
    main()
