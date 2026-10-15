import unittest
from test_control import Clock, FirmwareSerial
import test_control
from control import Controller


class AcLightingTests(unittest.TestCase):
    run_until_idle = test_control.ControlTests.run_until_idle
    connect_arm = test_control.ControlTests.connect_arm

    def setUp(self):
        self.clock = Clock()
        self.serial = FirmwareSerial()
        self.events = []
        self.c = Controller(self.clock, lambda k, d: self.events.append((k, d)))

    def advance(self, seconds):
        for _ in range(round(seconds * 100)):
            self.clock.now += .01
            self.c.tick()

    def test_ac_continues_for_three_minutes_without_manual_authorization(self):
        self.connect_arm()
        self.c.select_continuous([89])
        self.run_until_idle()
        self.advance(180)
        self.run_until_idle()
        self.assertTrue(self.c.armed)
        self.assertEqual(self.serial.mode, 'ac')
        self.assertGreater(sum(x == 'sys arm on 60' for x in self.serial.writes), 3)
        self.assertEqual(sum(x == 'hv mode ac --json' for x in self.serial.writes), 1)

    def test_renewal_never_rearms_firmware_after_it_has_stopped(self):
        self.connect_arm()
        self.serial.armed = False
        self.serial.mode = 'off'
        self.advance(25)
        self.run_until_idle()
        self.assertFalse(self.c.armed)
        self.assertEqual(sum(x == 'sys arm on 60' for x in self.serial.writes), 1)

    def test_renewal_checks_interlock_without_user_click(self):
        self.connect_arm()
        self.serial.interlock = False
        self.advance(25)
        self.run_until_idle()
        self.assertFalse(self.c.armed)
        self.assertEqual(sum(x == 'sys arm on 60' for x in self.serial.writes), 1)

    def test_long_sequence_completes_past_sixty_seconds(self):
        self.connect_arm()
        self.c.sequence([([1 if i % 2 == 0 else 2], 10000) for i in range(8)])
        self.run_until_idle(limit=12000)
        self.assertEqual(sum(x == 'hv mode ac --json' for x in self.serial.writes), 8)
        self.assertFalse(any(k == 'error' for k, _ in self.events))
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')

    def test_background_maintenance_does_not_flood_visible_serial_log(self):
        self.connect_arm()
        before = len([k for k, _ in self.events if k in ('tx', 'rx')])
        self.advance(45)
        self.run_until_idle()
        self.assertTrue(self.c.armed)
        self.assertEqual(len([k for k, _ in self.events if k in ('tx', 'rx')]), before)

    def test_light_commands_work_with_ac_disabled_and_validate_reply(self):
        self.c.connect(self.serial)
        self.run_until_idle()
        original_write = self.serial.write

        def write(data):
            if data.startswith(b'ledstrip '):
                self.serial.writes.append(data.decode().strip())
                self.serial.rx.extend(b'ledstrip effect=chase mode=running count=30 brightness=80 interval_ms=30\r\n')
                return len(data)
            return original_write(data)

        self.serial.write = write
        self.c.set_light('chase', 80, 30)
        self.run_until_idle()
        self.assertEqual(self.serial.writes[-1], 'ledstrip effect chase 30 80 30')
        self.assertFalse(self.c.armed)
        self.assertFalse(any('arm on' in x for x in self.serial.writes))
        self.assertTrue(any(k == 'light' for k, _ in self.events))

    def test_wrong_light_reply_does_not_report_success(self):
        self.c.connect(self.serial)
        self.run_until_idle()
        original_write = self.serial.write

        def write(data):
            if data.startswith(b'ledstrip '):
                self.serial.writes.append(data.decode().strip())
                self.serial.rx.extend(b'ledstrip effect=meteor mode=running count=30 brightness=80 interval_ms=30\r\n')
                return len(data)
            return original_write(data)

        self.serial.write = write
        self.c.set_light('chase', 80, 30)
        self.run_until_idle()
        self.assertFalse(any(k == 'light' for k, _ in self.events))
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')

    def test_invalid_light_parameters_do_not_write(self):
        self.c.connect(self.serial)
        self.run_until_idle()
        before = list(self.serial.writes)
        for args in [('bad', 80, 30), ('red', 256, 30), ('chase', 80, 0)]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                self.c.set_light(*args)
        self.assertEqual(self.serial.writes, before)

    def test_stop_during_maintenance_never_renews_after_stop(self):
        self.connect_arm()
        self.clock.now += 21
        self.c.tick()
        self.c.stop()
        self.run_until_idle()
        before = len(self.serial.writes)
        self.advance(80)
        self.assertFalse(self.c.armed)
        self.assertEqual(self.serial.writes[before:], [])
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')

    def test_silent_cable_loss_is_detected_during_maintenance(self):
        self.connect_arm()
        self.c.select_continuous([1])
        self.run_until_idle()
        self.serial.auto_reply = False
        self.advance(28)
        self.assertFalse(self.c.armed)
        self.assertFalse(self.c.ready)
        self.assertTrue(self.c.uncertain)

    def test_all_light_protocol_variants_and_fragmented_reply(self):
        from lighting import light_request, parse_light_reply, EFFECTS
        cases = [('red', 'ledstrip color 30 red 80',
                  'ledstrip color count=30 color=red brightness=80 rgb=80,0,0 ms=0'),
                 ('white', 'ledstrip color 30 white 80',
                  'ledstrip color count=30 color=white brightness=80 rgb=80,80,80 ms=0'),
                 ('off', 'ledstrip off 30', 'ledstrip off count=30 rgb=0,0,0'),
                 ('demo', 'ledstrip demo 30 80 30',
                  'ledstrip effect=rainbow mode=demo count=30 brightness=80 interval_ms=30')]
        cases += [(name, f'ledstrip effect {name} 30 80 30',
                   f'ledstrip effect={name} mode=running count=30 brightness=80 interval_ms=30')
                  for name in EFFECTS]
        for mode, cmd, reply in cases:
            with self.subTest(mode=mode):
                self.assertEqual(light_request(mode)[0], cmd)
                self.assertTrue(parse_light_reply(cmd, reply)['ok'])
        from control import Link, Command
        self.serial.auto_reply = False
        link = Link(self.serial, self.clock)
        link.send(Command('ledstrip off 30', 'ledstrip_text'))
        self.serial.rx.extend(b'ledstrip off count=30 rgb=0,0,0')
        self.assertIsNone(link.poll())
        self.serial.rx.extend(b'\r\n')
        self.assertEqual(link.poll()['cmd'], 'ledstrip')


if __name__ == '__main__':
    unittest.main()
