#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
[vkpkt] 记录侧离线核验（不依赖游戏/native）。

输入：TestVKPkt 跑出来的日志（stdout+stderr 合并）。
做四件事：
  1. [vkraw] 64 字节按 spec §3 的偏移/宽度解码，与 [vkcase] 的期望值逐字段比对；
     同时检查两个 reserved(u16@46 / u8@57 / u16@58 / u32@60) 恒为 0。
  2. [vkpkt] 行按 SPEC §2 的字段顺序解析，独立重算 canonical/sum 并与行内 sum 比对。
  3. 逐场景核对文本字段口径（n/v0/idx/slots/blend/rtParams/...）。
  4. 用 tools/vkaudit 的 recon 把 [vkpkt] 当 packet 侧、同内容改前缀当 exec 侧，
     要求 parser 接受且"摘要不符"为 0。

用法：
  python check_packets.py <test.log> [<vkaudit.py>]
"""
import hashlib
import re
import struct
import subprocess
import sys
from pathlib import Path

DIGEST_FIELDS = ["f", "prog", "rt", "prim", "n", "v0", "idx", "slots",
                 "blend", "depth", "dwrite", "scissor", "viewport", "u"]
U32_FIELDS = {"seq", "flags", "frame", "prog", "fbo", "ibo", "vbo",
              "clearColor", "srcLine", "rtParams"}


def parse_kv(text):
    return dict(re.findall(r"([a-z][a-z0-9]*)=([^ \r\n]+)", text))


def decode(pkt):
    """按 spec §3 解码 64 字节（小端）。"""
    assert len(pkt) == 64, f"packet 长度 {len(pkt)} != 64"
    seq, kind, flags, frame, prog, fbo, ibo, vbo = struct.unpack_from("<IHHIIIII", pkt, 0)
    first, count, color = struct.unpack_from("<iiI", pkt, 28)
    depth = struct.unpack_from("<f", pkt, 40)[0]
    clear_mask, state_ver, res16 = struct.unpack_from("<BBH", pkt, 44)
    src_line, rt_params = struct.unpack_from("<II", pkt, 48)
    fmt, res8, res16b = struct.unpack_from("<BBH", pkt, 56)
    res32 = struct.unpack_from("<I", pkt, 60)[0]
    return dict(seq=seq, kind=kind, flags=flags, frame=frame, prog=prog, fbo=fbo,
                ibo=ibo, vbo=vbo, first=first, count=count, clearColor=color,
                clearDepth=depth, clearMask=clear_mask, stateVersion=state_ver,
                res16=res16, srcLine=src_line, rtParams=rt_params,
                progFormat=fmt, res8=res8, res16b=res16b, res32=res32)


EXPECT_TEXT = {
    "S1_indexed_basic": dict(n="6", v0="-", idx="6", slots="2:3", blend="alpha",
                             depth="1", dwrite="1", scissor="0,0,921,691", prim="tris"),
    "S2_premultiplied_scissor_off": dict(n="3", v0="-", idx="3", slots="4:-", blend="premultiplied",
                                         depth="0", dwrite="0", scissor="0,0,0,0"),
    "S3_arrays_yflip": dict(n="18", v0="120", idx="-", slots="-", blend="add",
                            scissor="10,930,100,50"),
    "S4_explicit_clear": dict(n="0", v0="-", idx="-", slots="-"),
    "S5_implicit_clear": dict(n="0", v0="-", idx="-", slots="-"),
    "S6_render_to_texture": dict(n="12", v0="-", idx="12", slots="6", blend="none", rt="5"),
    "S7_subtract": dict(blend="subtract"),
    "S8_unknown_blend": dict(blend="x0302:0001:8006"),
    "S9_no_textures": dict(slots="-", n="3"),
    "S10_agal_arrays": dict(n="6", v0="0", idx="-", slots="3"),
    # 新增：program 切换后只取当前 program 声明的 2 个 sampler（不是残留的 3 个）
    "S11_program_switch_slots": dict(n="3", v0="-", idx="3", slots="2:3", blend="alpha",
                                     depth="1", dwrite="1", scissor="0,0,921,691"),
    # 新增：clear 行必须在记录前 flush，取到切换后的 prog/blend
    "S12_clear_after_blend_change": dict(n="0", v0="-", idx="-", slots="5:-", blend="add",
                                         prog="8", depth="1", dwrite="1"),
}

EXPECT_WIRE = {
    "S4_explicit_clear": dict(clearColor=0xFF00FF00, clearMask=3, srcLine=523, clearDepth=0.5),
    "S5_implicit_clear": dict(clearColor=0xFF000000, clearMask=1, srcLine=523, flags=0x07),
    "S6_render_to_texture": dict(rtParams=0x32),
    "S10_agal_arrays": dict(progFormat=1, srcLine=2028),
}


def main():
    log_path = Path(sys.argv[1])
    vkaudit = Path(sys.argv[2]) if len(sys.argv) > 2 else None

    lines = log_path.read_text(encoding="utf-8-sig", errors="replace").splitlines()

    cases = {}
    raws = {}
    logs = {}
    vk = {}
    for ln in lines:
        if ln.startswith("[vkcase]"):
            kv = parse_kv(ln)
            cases[int(kv["s"])] = kv
        elif ln.startswith("[vkraw]"):
            kv = parse_kv(ln)
            raws[int(kv["s"])] = kv
        elif ln.startswith("[vklog]"):
            tokens = ln.split(" ", 2)
            if len(tokens) == 3:
                kv = parse_kv(tokens[1])
                logs[int(kv["s"])] = tokens[2].strip()
        elif ln.startswith("[vkpkt]"):
            kv = parse_kv(ln)
            vk[int(kv["s"])] = (ln, kv)

    errs = []
    if not vk:
        errs.append("没有任何 [vkpkt] 行")
    if not raws:
        errs.append("没有任何 [vkraw] 行")

    print(f"[check] vkpkt={len(vk)} raw={len(raws)} case={len(cases)}")

    # --- 1. 线格式：偏移/宽度 ---
    for s, (ln, kv) in sorted(vk.items()):
        if s not in raws:
            errs.append(f"s={s}: 缺 [vkraw]")
            continue
        tokens = [t for t in lines if t.startswith("[vkraw]") and f"s={s} " in t]
        if not tokens:
            errs.append(f"s={s}: 缺 [vkraw] 行")
            continue
        pkt = bytes.fromhex(tokens[0].split()[-1])
        d = decode(pkt)
        exp = cases.get(s)
        if exp is None:
            errs.append(f"s={s}: 缺 [vkcase]")
            continue
        for field in ("seq", "kind", "flags", "frame", "prog", "fbo", "ibo", "vbo",
                      "first", "count", "clearColor", "clearMask", "srcLine", "rtParams",
                      "progFormat"):
            key_case = "f" if field == "frame" else ("rt" if field == "fbo" else
                                                     ("clearDepth" if field == "clearDepth" else field))
            if key_case not in exp:
                continue
            want = int(exp[key_case])
            got = int(d[field])
            if field in U32_FIELDS:
                want &= 0xFFFFFFFF
                got &= 0xFFFFFFFF
            if got != want:
                errs.append(f"s={s}: 线字段 {field} 解码={got} 期望={want}")
        if "clearDepth" in exp:
            if abs(d["clearDepth"] - float(exp["clearDepth"])) > 1e-6:
                errs.append(f"s={s}: clearDepth 解码={d['clearDepth']} 期望={exp['clearDepth']}")
        # reserved 必须为 0
        if d["res16"] != 0 or d["res8"] != 0 or d["res16b"] != 0 or d["res32"] != 0:
            errs.append(f"s={s}: reserved 非 0 {d['res16']},{d['res8']},{d['res16b']},{d['res32']}")
        # stateVersion：本分叉固定 0（native-only，Haxe 无 FFI 可取）
        if d["stateVersion"] != 0:
            errs.append(f"s={s}: stateVersion={d['stateVersion']}")
        # kind 与 [vkcase] 一致
        if d["kind"] != int(exp["kind"]):
            errs.append(f"s={s}: kind 不一致")

    # --- 2. 日志行：顺序 + 独立 sum ---
    for s, (ln, kv) in sorted(vk.items()):
        order = re.findall(r"([a-z][a-z0-9]*)=", ln)
        body = [f for f in order if f not in ("s", "sum")]
        if body != DIGEST_FIELDS:
            errs.append(f"s={s}: 字段顺序 {body} != {DIGEST_FIELDS}")
        if len(kv.get("u", "")) != 8 or not re.fullmatch(r"[0-9a-f]{8}", kv.get("u", "")):
            errs.append(f"s={s}: u 不是 8 位小写 hex: {kv.get('u')}")
        if not re.fullmatch(r"[0-9a-f]{12}", kv.get("sum", "")):
            errs.append(f"s={s}: sum 不是 12 位小写 hex: {kv.get('sum')}")
        canon = ";".join(f"{f}={kv[f]}" for f in DIGEST_FIELDS)
        want = hashlib.sha256(canon.encode("utf-8")).hexdigest()[:12]
        if want != kv.get("sum"):
            errs.append(f"s={s}: sum 不符 重算={want} 行内={kv.get('sum')}")
        # [vklog] 必须与 [vkpkt] 同文
        if s in logs and logs[s] != ln:
            errs.append(f"s={s}: [vklog] 与 [vkpkt] 内容不一致")

        # 场景文本口径
        name = cases.get(s, {}).get("name", "")
        for f, want_v in EXPECT_TEXT.get(name, {}).items():
            if kv.get(f) != want_v:
                errs.append(f"{name}: {f}={kv.get(f)} 期望 {want_v}")
        for f, want_v in EXPECT_WIRE.get(name, {}).items():
            pkt = bytes.fromhex([t for t in lines if t.startswith("[vkraw]") and f"s={s} " in t][0].split()[-1])
            d = decode(pkt)
            if d[f] != want_v:
                errs.append(f"{name}: 线字段 {f}={d[f]} 期望 {want_v}")

    # --- 3. 场景数与 helper 口径 ---
    helper = next((l for l in lines if l.startswith("[vkhelp]")), "")
    want_clear = str(0xFF996633)
    for token in ("scissor_off=0,0,0,0", "scissor_yflip=10,930,100,50",
                  "rtparams_backbuffer_ds=32", f"clearcolor={want_clear}",
                  "slots=1:-:3", "hexneg=ffffffff",
                  "slot_pad=7:-", "slot_trunc=2:3", "slot_empty=-"):
        if token not in helper:
            errs.append(f"helper 口径不符: 缺 {token}（实际 {helper}）")
    end = next((l for l in lines if l.startswith("[vkend]")), "")
    if "packets=12" not in end:
        errs.append(f"场景数不符: {end}")

    # --- 3b. 新增用例是否真能复现旧口径 ---
    stale = {}
    for ln in lines:
        if ln.startswith("[vkstale]"):
            kv = parse_kv(ln)
            if "name" in kv:
                stale[kv["name"]] = kv

    def case_of(name):
        for s, (_, kv) in vk.items():
            if cases.get(s, {}).get("name") == name:
                return s, kv
        return None, None

    # 用例 1：旧口径取 textures 数组长度 → 2:3:4；新口径按 program 声明数 → 2:3
    s11, kv11 = case_of("S11_program_switch_slots")
    if stale.get("S11", {}).get("slots") != "2:3:4":
        errs.append(f"S11 旧口径基线不符: {stale.get('S11', {}).get('slots')} 期望 2:3:4")
    if kv11 is not None:
        if kv11.get("slots") == stale.get("S11", {}).get("slots"):
            errs.append("S11 新旧口径 slots 相同，未复现跨 program 尾项问题")
        else:
            print(f"[check] S11 旧口径 slots=2:3:4 新口径 slots={kv11.get('slots')}（旧口径会多尾项）")

    # 用例 2：旧口径（flush 前）prog=3/blend=alpha；新口径 prog=8/blend=add，sum 必然不同
    s12, kv12 = case_of("S12_clear_after_blend_change")
    if kv12 is not None:
        old = dict(kv12)
        old["prog"] = stale.get("S12", {}).get("prog", "")
        old["blend"] = stale.get("S12", {}).get("blend", "")
        old_sum = hashlib.sha256(";".join(f"{f}={old[f]}" for f in DIGEST_FIELDS).encode("utf-8")).hexdigest()[:12]
        if old_sum == kv12.get("sum"):
            errs.append("S12 新旧口径 sum 相同，未复现 clear 前未 flush 的问题")
        else:
            print(f"[check] S12 旧口径(prog=3/blend=alpha) sum={old_sum} 新口径 sum={kv12.get('sum')}（旧口径会与 native 不符）")

    # --- 4. vkaudit recon ---
    if vkaudit is not None and vkaudit.exists():
        pkt_log = log_path.with_suffix(".packets.log")
        exec_log = log_path.with_suffix(".exec.log")
        report = log_path.with_suffix(".recon.md")
        pkt_log.write_text("\n".join(l for l, _ in vk.values()) + "\n", encoding="utf-8")
        exec_log.write_text("\n".join(l.replace("[vkpkt]", "[vkexec]", 1) for l, _ in vk.values()) + "\n",
                            encoding="utf-8")
        print(f"[check] vkaudit recon: {pkt_log.name} vs {exec_log.name}")
        r = subprocess.run([sys.executable, str(vkaudit), "recon", "--out", str(report),
                            str(pkt_log), str(exec_log)],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        print(r.stdout.strip()[:1200])
        if r.returncode != 0:
            errs.append(f"vkaudit recon 退出码 {r.returncode}: {r.stderr.strip()[:300]}")
        if "摘要不符" in r.stdout and "摘要不符 | 0" not in r.stdout and "摘要不符 |0" not in r.stdout:
            # 输出表里若有非 0 摘要不符，视为失败
            m = re.search(r"摘要不符[^\n]*?\| *(\d+)", r.stdout)
            if m and int(m.group(1)) != 0:
                errs.append(f"vkaudit: 摘要不符={m.group(1)}")
    else:
        print("[check] 未提供 vkaudit.py，跳过 recon")

    if errs:
        print("\n[check] FAIL")
        for e in errs:
            print("  - " + e)
        return 1

    print("\n[check] PASS：线偏移/宽度、字段顺序、独立 sum、场景口径、vkaudit 解析全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
