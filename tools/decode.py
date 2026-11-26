#!/usr/bin/env python3
"""Punto 188 B-CAN CLI decoder — stdlib only, agentic friendly.
Unified merged log from ESP32 /csv: t_ms,src,id,dlc,data
  CAN rows: src=CAN, id=0DD (hex), dlc=7, data=hex payload (e.g. 00007200028019)
  OBD rows: src=OBD, id=010C|ATRV, dlc=0, data=decoded value (e.g. 816.5)
Legacy split files (ms,id,dlc,b0..b7... / t_ms,pid,value) still read.

Engine on is RPM > 0 (0DD.b6 > 0); 0DD.b0/b1 == 0 also occurs at key-on.

  python3 decode.py summary log.csv
  python3 decode.py bits base.csv test.csv       # what changed vs baseline
  python3 decode.py ref log.csv [obd.csv]        # auto-fit CAN formulas vs OBD
  python3 decode.py candump log.csv > out.asc    # convert to candump -L
"""
import argparse, csv, math
from collections import defaultdict

DIAG_IDS = {'0C6', '0C7'}  # Fiat CODE immobilizer keepalive, never drive signals

def _is_merged(fieldnames):
    return fieldnames and 'src' in fieldnames and 'data' in fieldnames

def load_log(path):
    """-> (can_rows, obd_rows). can: {ms,id,dlc,d[]}. obd: {ms,pid,val}."""
    can, obd = [], []
    with open(path) as f:
        rd = csv.DictReader(f)
        fn = rd.fieldnames or []
        if _is_merged(fn):
            for r in rd:
                try:
                    src = (r.get('src') or '').strip().upper()
                    if src == 'CAN':
                        i = r['id'].strip().upper()
                        if i in DIAG_IDS: continue
                        hx = (r.get('data') or '').strip()
                        d = [int(hx[k:k+2], 16) for k in range(0, len(hx), 2)]
                        can.append({'ms': int(r['t_ms']), 'id': i,
                                    'dlc': int(r['dlc'] or len(d)), 'd': d})
                    elif src == 'OBD':
                        obd.append({'ms': int(r['t_ms']),
                                    'pid': norm_pid(r['id']),
                                    'val': float(r['data'])})
                except (ValueError, KeyError, TypeError):
                    continue
        elif 't_ms' in fn and 'pid' in fn:  # legacy OBD split file
            for r in rd:
                try:
                    obd.append({'ms': int(r['t_ms']),
                                'pid': norm_pid(r['pid']),
                                'val': float(r['value'])})
                except (ValueError, KeyError, TypeError):
                    continue
        else:  # legacy CAN split file
            for r in rd:
                try:
                    dlc = int(r['dlc'])
                    d = [int(r[f'b{i}'], 16) for i in range(dlc)]
                except Exception:
                    continue
                i = r['id'].strip().upper()
                if i in DIAG_IDS: continue
                can.append({'ms': int(r['ms']), 'id': i, 'dlc': dlc, 'd': d})
    return can, obd

def norm_pid(p):
    p = (p or '').strip().upper()
    if p == 'ATRV': return p
    if len(p) == 4 and p.startswith('01'): return p[2:]
    return p

def load_can(path):
    can, _ = load_log(path)
    return can

def load_obd(path):
    _, obd = load_log(path)
    return obd

def obd_index(obd):
    ref = defaultdict(list)
    for r in obd:
        ref[r['pid']].append((r['ms'], r['val']))
    return ref

def cmd_summary(path):
    rows = load_can(path)
    if not rows: print('empty'); return
    t0, t1 = rows[0]['ms'], rows[-1]['ms']
    span = max(1, t1 - t0) / 1000
    by = defaultdict(list)
    for r in rows: by[(r['id'], r['dlc'])].append(r)
    print(f"{'ID':>5} {'dlc':>3} {'count':>6} {'per_s':>6} {'changing_bytes':<16}")
    print(f"-- {len(rows)} CAN frames over {span:.0f}s "
          f"({len(rows)/span:.0f}/s of 4000-ring)")
    for (i, dlc), v in sorted(by.items()):
        ch = [str(b) for b in range(dlc) if len({p['d'][b] for p in v}) > 1]
        print(f"{i:>5} {dlc:>3} {len(v):>6} {len(v)/span:>6.1f} {','.join(ch):<16}")
    _, obd = load_log(path)
    if obd:
        idx = obd_index(obd)
        print(f"-- {len(obd)} OBD rows: " +
              ', '.join(f"{p}:{len(v)}" for p, v in sorted(idx.items())))

def cmd_bits(base, test):
    A = load_can(base)
    seen = defaultdict(set)
    for r in load_can(test): seen[(r['id'], r['dlc'])].add(tuple(r['d']))
    print(f"--- frames in {test} not in {base} ---")
    for (i, dlc), payloads in sorted(seen.items()):
        base_p = {tuple(r['d']) for r in A if (r['id'], r['dlc']) == (i, dlc)}
        if not base_p: print(f"{i} NEW ID (first seen here)"); continue
        new = [p for p in payloads if p not in base_p]
        if not new: continue
        ref = next(iter(base_p))
        print(f"{i} dlc={dlc} new_payloads={len(new)} e.g. {' '.join('%02X'%b for b in new[0])}")
        for p in list(new)[:3]:
            print(f"  vs base {' '.join('%02X'%b for b in ref)} "
                  f"changed bytes: {','.join(str(b) for b in range(dlc) if p[b]!=ref[b])}")

def linreg(pairs):
    n = len(pairs)
    if n < 3: return None
    sx = sum(x for x, _ in pairs); sy = sum(y for _, y in pairs)
    mx, my = sx / n, sy / n
    sxx = sum((x - mx) ** 2 for x, _ in pairs)
    if sxx == 0: return None
    sxy = sum((x - mx) * (y - my) for x, y in pairs)
    syy = sum((y - my) ** 2 for _, y in pairs)
    slope = sxy / sxx
    icept = my - slope * mx
    rmse = math.sqrt(sum((slope * x + icept - y) ** 2 for x, y in pairs) / n)
    mae = sum(abs(slope * x + icept - y) for x, y in pairs) / n
    r = sxy / math.sqrt(sxx * syy) if syy > 0 else 0.0
    return dict(n=n, slope=slope, icept=icept, rmse=rmse, mae=mae, r=r,
                xmin=min(x for x, _ in pairs), xmax=max(x for x, _ in pairs))

def _pairs(rows, ref, cid, fn, pid, maxgap=4000):
    pts = sorted(ref.get(pid, []))
    if not pts: return []
    out, j = [], 0
    for r in rows:
        if r['id'] != cid: continue
        try:
            raw = fn(r)
        except IndexError:
            continue
        if raw is None: continue
        while j + 1 < len(pts) and pts[j + 1][0] <= r['ms']: j += 1
        best = min(pts[max(0, j - 1):j + 2], key=lambda p: abs(p[0] - r['ms']))
        if abs(best[0] - r['ms']) <= maxgap: out.append((raw, best[1]))
    return out

def _show_fit(label, pairs, unit, exact=None):
    f = linreg(pairs)
    if not f:
        print(f"{label}: not enough overlap"); return
    s = (f"{label}: val = raw*{f['slope']:.4f}+{f['icept']:+.1f} {unit} "
         f"(r={f['r']:.4f} RMSE={f['rmse']:.1f} MAE={f['mae']:.1f} "
         f"n={f['n']} raw {f['xmin']:.0f}..{f['xmax']:.0f})")
    if exact:
        ef = exact(pairs)
        s += f" | exact-fit RMSE={ef:.1f}"
    print(s)

def _exact_rmse(pairs, slope, icept):
    import math as _m
    return _m.sqrt(sum((slope * x + icept - y) ** 2 for x, y in pairs) / len(pairs))

def cmd_ref(can_path, obd_path=None):
    can, obd_merged = load_log(can_path)
    ref = obd_index(obd_merged)
    if obd_path:
        for r in load_obd(obd_path):
            ref[r['pid']].append((r['ms'], r['val']))
    if not ref:
        print('no OBD ground truth — capture with ELM connected'); return
    for pid, pts in sorted(ref.items()):
        print(f"OBD {pid}: {len(pts)} pts range {min(v for _,v in pts):.1f}..{max(v for _,v in pts):.1f}")

    # RPM = 0DD.b6 * 32 (same law as the 1.4 16V "byte 6 = rpm/32"). The 16-bit
    # (b6<<8|b5)/8 read is algebraically b6*32 + b5/8; b5 is the LOW BYTE of the
    # 16-bit BE FuelInjectionPulse (b4:b5), so the 8-bit law stays primary and
    # the composite is reported as a cross-check only.
    rpm = _pairs(can, ref, '0DD',
                 lambda r: r['d'][6] * 32 if r['dlc'] >= 7 else None, '0C')
    _show_fit('RPM  0DD.b6*32', rpm, 'rpm',
              exact=lambda p: _exact_rmse(p, 1.0, 0) if p else float('nan'))
    rpm16 = _pairs(can, ref, '0DD',
                   lambda r: ((r['d'][6] << 8) | r['d'][5]) / 8 if r['dlc'] >= 7 else None, '0C')
    _show_fit('RPM  0DD(b6:b5)/8 [x-check]', rpm16, 'rpm',
              exact=lambda p: _exact_rmse(p, 1.0, 0) if p else float('nan'))
    cool = _pairs(can, ref, '0DD',
                  lambda r: r['d'][2] if r['dlc'] >= 3 else None, '05')
    _show_fit('COOL 0DD.b2    ', cool, 'C',
              exact=lambda p: _exact_rmse(p, 1.0, -40) if p else float('nan'))

    # load/state byte vs K-line load signals (b4 is the HIGH byte of the
    # FuelInjectionPulse b4:b5 pair; DFCO drops the pair to 0x0000)
    for pid, nm in (('04', 'LOAD'), ('0B', 'MAP '), ('11', 'THR ')):
        _show_fit(f'B4 vs {nm} 0DD.b4',
                  _pairs(can, ref, '0DD',
                           lambda r: r['d'][4] if r['dlc'] >= 5 else None, pid), '%')

    # Speed hunt: every 8-bit + LE16/BE16 pair of every ID vs 010D.
    if '0D' in ref and any(v != ref['0D'][0][1] for _, v in ref['0D']):
        cands, watch = [], {}
        for cid in sorted({r['id'] for r in can}):
            dlc = next(r['dlc'] for r in can if r['id'] == cid)
            cands_here = []
            for b in range(dlc):
                f = linreg(_pairs(can, ref, cid, lambda r, b=b: r['d'][b], '0D'))
                if f and abs(f['r']) > 0.5:
                    cands.append((abs(f['r']), f"{cid}.b{b}", f))
                    cands_here.append((abs(f['r']), f"{cid}.b{b}", f))
            for b in range(dlc - 1):
                for nm, fn in (("LE", lambda r, b=b: r['d'][b] | r['d'][b+1] << 8),
                                ("BE", lambda r, b=b: r['d'][b] << 8 | r['d'][b+1])):
                    f = linreg(_pairs(can, ref, cid, fn, '0D'))
                    if f and abs(f['r']) > 0.5:
                        cands.append((abs(f['r']), f"{cid}.b{b}:{b+1}{nm}", f))
                        cands_here.append((abs(f['r']), f"{cid}.b{b}:{b+1}{nm}", f))
            if cid in ('0DD', '0E5') and cands_here:
                cands_here.sort(reverse=True)
                _, nm, f = cands_here[0]
                watch[cid] = (nm, f)
        cands.sort(reverse=True)
        print('SPEED hunt vs 010D (top 5):')
        for _, nm, f in cands[:5]:
            print(f"  {nm}: val = raw*{f['slope']:.4f}+{f['icept']:+.1f} "
                  f"(r={f['r']:.4f} RMSE={f['rmse']:.1f} n={f['n']})")
        if not cands: print('  no byte tracks 010D (|r|<0.5) — was the car moving?')
        for cid in ('0DD', '0E5'):
            if cid in watch:
                nm, f = watch[cid]
                print(f"  watch {nm}: r={f['r']:.3f} RMSE={f['rmse']:.1f} n={f['n']}")
    elif '0D' in ref:
        print('SPEED: 010D flat (car stationary) — hunt needs a driving run')

def cmd_candump(path):
    for r in load_can(path):
        data = ''.join(f'{b:02X}' for b in r['d'])
        print(f"({r['ms']/1000:.3f}) can0 {r['id']}#{data}")

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='c', required=True)
    s = sub.add_parser('summary'); s.add_argument('csv')
    b = sub.add_parser('bits'); b.add_argument('base'); b.add_argument('test')
    r = sub.add_parser('ref'); r.add_argument('can'); r.add_argument('obd', nargs='?')
    c = sub.add_parser('candump'); c.add_argument('csv')
    a = ap.parse_args()
    if a.c == 'summary': cmd_summary(a.csv)
    elif a.c == 'bits': cmd_bits(a.base, a.test)
    elif a.c == 'ref': cmd_ref(a.can, a.obd)
    elif a.c == 'candump': cmd_candump(a.csv)

if __name__ == '__main__': main()
