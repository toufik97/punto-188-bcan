#!/usr/bin/env python3
"""Tests for punto_188_8v.dbc — stdlib only, no cantools needed.
Run: python3 test_dbc.py
Covers: file parses, no overlapping signals, bits fit DLC, no 0C6/0C7,
and known captured frames decode to verified ground truth.
"""
import os
import re
import unittest

DBC = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   '..', 'dbc', 'punto_188_8v.dbc'))

BO_RE = re.compile(r'^BO_\s+(\d+)\s+(\w+):\s+(\d+)\s+(\w+)')
SG_RE = re.compile(r'^\s*SG_\s+(\w+)\s*:\s*(\d+)\|(\d+)@([01])([+-])'
                   r'\s*\(\s*([^,]+)\s*,\s*([^\)]+)\)'
                   r'\s*\[([^\|]+)\|([^\]]+)\]\s*"([^"]*)"')


def getbit(payload, n):
    return (payload[n // 8] >> (n % 8)) & 1 if n // 8 < len(payload) else 0


def raw_of(sig, payload):
    start, ln, order = sig['start'], sig['len'], sig['order']
    if order == 1:  # little-endian: linear from LSB
        return sum(getbit(payload, start + i) << i for i in range(ln))
    raw, pos = 0, start  # big-endian: MSB first, walk down, jump +15 at byte edge
    for _ in range(ln):
        raw = (raw << 1) | getbit(payload, pos)
        pos = pos - 1 if pos % 8 > 0 else pos + 15
    return raw


def covered(sig, dlc):
    out, pos = set(), sig['start']
    for _ in range(sig['len']):
        if pos < 0 or pos >= dlc * 8:
            raise ValueError(f"{sig['name']} bit {pos} outside {dlc} bytes")
        out.add(pos)
        if sig['order'] == 1:
            pos += 1
        else:
            pos = pos - 1 if pos % 8 > 0 else pos + 15
    return out


def load_dbc(path=DBC):
    msgs, cur = {}, None
    with open(path) as f:
        for line in f:
            m = BO_RE.match(line)
            if m:
                cur = {'id': int(m.group(1)), 'name': m.group(2),
                       'dlc': int(m.group(3)), 'sigs': []}
                msgs[cur['id']] = cur
                continue
            s = SG_RE.match(line)
            if s and cur is not None:
                cur['sigs'].append({
                    'name': s.group(1), 'start': int(s.group(2)),
                    'len': int(s.group(3)), 'order': int(s.group(4)),
                    'signed': s.group(5) == '-',
                    'factor': float(s.group(6)), 'offset': float(s.group(7)),
                })
    return msgs


def decode(msgs, can_id, payload):
    m = msgs[can_id]
    assert len(payload) >= m['dlc'], f'short payload for {can_id}'
    out = {}
    for s in m['sigs']:
        out[s['name']] = raw_of(s, payload) * s['factor'] + s['offset']
    return out


class TestDbc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = load_dbc()

    def test_expected_ids(self):
        self.assertEqual(set(self.m),
                         {221, 240, 229, 207, 243, 242, 1792, 1793, 1794, 1795})

    def test_no_diagnostic_visitors(self):
        self.assertNotIn(0x0C6, self.m)
        self.assertNotIn(0x0C7, self.m)

    def test_no_overlaps_and_fit(self):
        for i, m in self.m.items():
            seen = set()
            for s in m['sigs']:
                cov = covered(s, m['dlc'])
                self.assertTrue(seen.isdisjoint(cov))
                seen |= cov

    def test_rpm_coolant_idle(self):
        # real idle frame: 0DD 00 00 72 00 01 E6 19 (OBD said ~816 rpm, ~74 C)
        v = decode(self.m, 221, [0x00, 0x00, 0x72, 0x00, 0x01, 0xE6, 0x19])
        self.assertAlmostEqual(v['RPM'], 6630 / 8)
        self.assertAlmostEqual(v['Coolant'], 0x72 - 40)
        self.assertEqual(v['LoadRaw'], 1)

    def test_doors_fuel_handbrake(self):
        # driver door open, handbrake on, fuel 0x1C
        v = decode(self.m, 240, [0x28, 0x84, 0x40, 0x00, 0x00, 0x1C, 0x0D, 0x00])
        self.assertEqual(v['Handbrake'], 1)
        self.assertEqual(v['DoorDrv'], 1)
        self.assertEqual(v['DoorPsg'], 0)
        self.assertEqual(v['DoorRearLeft'], 0)
        self.assertEqual(v['DoorRearRight'], 0)
        self.assertEqual(v['BootOpen'], 0)
        self.assertEqual(v['FuelSender'], 0x1C)

    def test_speed(self):
        # 0E5 01 9A 40 .. = 0x019A = 410 -> 41.0 km/h
        v = decode(self.m, 229, [0x01, 0x9A, 0x40, 0x00])
        self.assertAlmostEqual(v['Speed'], 41.0)

    def test_lights(self):
        # 0CF 00 6A 20 .. = LOW+FOG+position, right blinker
        v = decode(self.m, 207, [0x00, 0x6A, 0x20, 0x00, 0x00, 0x00])
        self.assertEqual(v['FootBrake'], 0)
        self.assertEqual(v['LightFog'], 1)
        self.assertEqual(v['LightLow'], 1)
        self.assertEqual(v['LightHigh'], 0)
        self.assertEqual(v['LightPosA'], 1)
        self.assertEqual(v['LightPosB'], 1)
        self.assertEqual(v['BlinkRight'], 1)
        self.assertEqual(v['BlinkLeft'], 0)
        self.assertEqual(v['Defog'], 0)

    def test_city_handbrake_echo(self):
        v = decode(self.m, 242, [0x28])
        self.assertEqual(v['City'], 1)
        v = decode(self.m, 242, [0x08])
        self.assertEqual(v['City'], 0)
        v = decode(self.m, 243, [0xA0, 0x00, 0x00])
        self.assertEqual(v['Handbrake'], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
