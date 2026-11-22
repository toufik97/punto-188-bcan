#!/usr/bin/env python3
"""Tests for decode.py — synthetic merged-CSV fixture, stdlib unittest only.
Run: python3 test_decode.py
Covers: merged CAN/OBD split, 0C6/0C7 exclusion, RPM divisor recovery,
coolant curve recovery, speed-hunt plant, legacy split formats.
"""
import csv
import os
import tempfile
import unittest

import decode


def write_csv(path, header, rows):
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def can_row(ms, i, d):
    return [ms, 'CAN', i, len(d) // 2, d]


def obd_row(ms, pid, val):
    return [ms, 'OBD', pid, 0, val]


def make_log(path, obd_every=1):
    """Synthetic drive: rpm 800->3000, coolant 70->90, speed 0->50.
    CAN 0DD: b2=cool+40, b5:b6 LE16=rpm*8. Speed planted in 0E5.b3.
    OBD ground truth every `obd_every` steps."""
    hdr = ['t_ms', 'src', 'id', 'dlc', 'data']
    rows = []
    ms = 0
    for k in range(60):
        rpm = 800 + k * (2200 / 59)
        cool = 70 + k * (20 / 59)
        spd = k * (50 / 59)
        ms += 50  # 0DD @20Hz
        raw = int(rpm * 8)
        d = '0000%02X0000%02X%02X' % (int(cool + 40), raw & 0xFF,
                                      (raw >> 8) & 0xFF)
        rows.append(can_row(ms, '0DD', d))
        if k % 2 == 0:  # 0E5 @10Hz with planted speed byte (b3)
            rows.append(can_row(ms, '0E5', '000040%02X' % int(spd)))
        if k % obd_every == 0:  # OBD ground truth interleaved
            rows.append(obd_row(ms, '010C', round(rpm, 1)))
            rows.append(obd_row(ms, '0105', round(cool, 1)))
            rows.append(obd_row(ms, '010D', round(spd, 1)))
    # diagnostic visitors must be ignored everywhere
    rows.append(can_row(ms + 50, '0C6', 'AB1A3C00000000'))
    rows.append(can_row(ms + 100, '0C7', '06%s' % ('00' * 6)))
    write_csv(path, hdr, rows)


class TestMergedLog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d = tempfile.mkdtemp(prefix='puntocan_test_')
        cls.log = os.path.join(cls.d, 'merged.csv')
        make_log(cls.log, obd_every=1)    # dense: exact math recovery
        cls.sparse = os.path.join(cls.d, 'sparse.csv')
        make_log(cls.sparse, obd_every=10)  # sparse: realistic ELM cadence

    def test_split(self):
        can, obd = decode.load_log(self.log)
        self.assertTrue(all(r['id'] not in ('0C6', '0C7') for r in can))
        pids = {r['pid'] for r in obd}
        self.assertEqual(pids, {'0C', '05', '0D'})

    def test_diag_excluded(self):
        can, _ = decode.load_log(self.log)
        self.assertFalse({'0C6', '0C7'} & {r['id'] for r in can})

    def test_rpm_divisor(self):
        can, obd = decode.load_log(self.log)
        ref = decode.obd_index(obd)
        pairs = decode._pairs(
            can, ref, '0DD',
            lambda r: (r['d'][6] << 8 | r['d'][5]) if r['dlc'] >= 7 else None,
            '0C')
        f = decode.linreg(pairs)
        self.assertIsNotNone(f)
        self.assertAlmostEqual(f['slope'], 1 / 8, places=2)
        self.assertGreater(f['r'], 0.999)

    def test_coolant_curve(self):
        can, obd = decode.load_log(self.log)
        ref = decode.obd_index(obd)
        pairs = decode._pairs(
            can, ref, '0DD',
            lambda r: r['d'][2] if r['dlc'] >= 3 else None, '05')
        f = decode.linreg(pairs)
        self.assertIsNotNone(f)
        self.assertAlmostEqual(f['slope'], 1.0, delta=0.03)
        self.assertAlmostEqual(f['icept'], -40.0, delta=1.0)

    def test_speed_hunt_finds_plant(self):
        can, obd = decode.load_log(self.log)
        ref = decode.obd_index(obd)
        f = decode.linreg(decode._pairs(
            can, ref, '0E5', lambda r: r['d'][3], '0D'))
        self.assertIsNotNone(f)
        self.assertGreater(abs(f['r']), 0.999)
        self.assertAlmostEqual(f['slope'], 1.0, places=2)

    def test_sparse_pipeline_robust(self):
        # Realistic ELM cadence: staircase pairing must still roughly fit.
        can, obd = decode.load_log(self.sparse)
        ref = decode.obd_index(obd)
        rpm = decode.linreg(decode._pairs(
            can, ref, '0DD',
            lambda r: (r['d'][6] << 8 | r['d'][5]) if r['dlc'] >= 7 else None,
            '0C'))
        self.assertIsNotNone(rpm)
        self.assertGreater(abs(rpm['r']), 0.9)
        self.assertAlmostEqual(rpm['slope'], 1 / 8, delta=0.03)
        cool = decode.linreg(decode._pairs(
            can, ref, '0DD',
            lambda r: r['d'][2] if r['dlc'] >= 3 else None, '05'))
        self.assertIsNotNone(cool)
        self.assertGreater(abs(cool['r']), 0.9)
        spd = decode.linreg(decode._pairs(
            can, ref, '0E5', lambda r: r['d'][3], '0D'))
        self.assertIsNotNone(spd)
        self.assertGreater(abs(spd['r']), 0.9)

    def test_exact_rmse_helpers(self):
        self.assertAlmostEqual(
            decode._exact_rmse([(8.0, 1.0), (16.0, 2.0)], 1 / 8, 0), 0.0)

    def test_legacy_can(self):
        p = os.path.join(self.d, 'old_can.csv')
        write_csv(p, ['ms', 'id', 'dlc', 'b0', 'b1', 'b2', 'b3', 'b4', 'b5',
                      'b6', 'b7', 'rpm_est', 'cool_est'],
                  [[1000, '0DD', 7, '00', '00', '6E', '00', '01', '15', '19',
                    '00', 800, 74]])
        rows = decode.load_can(p)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['d'][2], 0x6E)

    def test_legacy_obd(self):
        p = os.path.join(self.d, 'old_obd.csv')
        write_csv(p, ['t_ms', 'pid', 'value'], [[1000, '010C', 816.0]])
        rows = decode.load_obd(p)
        self.assertEqual(rows[0]['pid'], '0C')


if __name__ == '__main__':
    unittest.main(verbosity=2)
