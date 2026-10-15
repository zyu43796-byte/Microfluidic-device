import json
import unittest
import test_control
from control import Controller


class MagnetSerial(test_control.FirmwareSerial):
    def __init__(self):
        super().__init__()
        self.wrong_state = False

    def write(self, data):
        command = data.decode().strip()
        if command.startswith('magnet ') and self.auto_reply:
            self.writes.append(command)
            state = command.split()[1]
            if self.wrong_state:
                state = 'invalid'
            self.rx.extend((json.dumps(dict(ok=True, cmd='magnet', state=state)) + '\n').encode())
            return len(data)
        return super().write(data)


class MagnetTests(unittest.TestCase):
    run_until_idle = test_control.ControlTests.run_until_idle
    connect_arm = test_control.ControlTests.connect_arm

    def setUp(self):
        self.clock = test_control.Clock()
        self.serial = MagnetSerial()
        self.events = []
        self.c = Controller(self.clock, lambda k, v: self.events.append((k, v)))
        self.c.connect(self.serial)
        self.run_until_idle()
        self.serial.writes.clear()

    def start_magnet(self, mode='on', duration=500):
        self.assertTrue(callable(getattr(self.c, 'set_magnet', None)), '磁杆控制入口尚未实现')
        self.c.set_magnet(mode, duration)
        self.run_until_idle()

    def advance(self, seconds):
        for _ in range(round(seconds * 100)):
            self.clock.now += .01
            self.c.tick()

    def test_on_independent_of_ac_and_no_hv_writes(self):
        self.start_magnet()
        self.assertTrue(self.c.magnet_active)
        self.assertFalse(self.c.armed)
        self.assertIn('sys arm on 60', self.serial.writes)
        self.assertIn('magnet on --json', self.serial.writes)
        self.assertFalse(any(s.startswith(('hv ', 'array ', 'heater ')) for s in self.serial.writes))

    def test_open_or_bypassed_interlock_blocks_on(self):
        for attr in ('interlock', 'bypass'):
            setattr(self.serial, attr, attr == 'bypass')
            self.start_magnet()
            self.assertNotIn('magnet on --json', self.serial.writes)
            self.serial.interlock, self.serial.bypass = True, False

    def test_off_allowed_with_open_interlock_and_disarms_standalone(self):
        self.start_magnet()
        self.serial.interlock = False
        self.start_magnet('off')
        self.assertIn('magnet off --json', self.serial.writes)
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')
        self.assertFalse(self.c.magnet_active)
        self.assertFalse(self.serial.armed)

    def test_timed_action_uses_nonblocking_on_off(self):
        self.start_magnet('timed', 500)
        self.assertEqual([s for s in self.serial.writes if s.startswith('magnet ')],
                         ['magnet on --json', 'magnet off --json'])
        self.assertFalse(self.c.magnet_active)

    def test_timed_action_can_be_interrupted(self):
        self.assertTrue(callable(getattr(self.c, 'set_magnet', None)), '磁杆控制入口尚未实现')
        self.c.set_magnet('timed', 5000)
        self.advance(.8)
        self.assertTrue(self.c.magnet_active)
        start = self.clock.now
        self.c.stop()
        self.run_until_idle()
        self.assertLess(self.clock.now - start, .2)
        self.assertFalse(self.c.magnet_active)
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')

    def test_wrong_ack_fails_closed(self):
        self.serial.wrong_state = True
        self.start_magnet()
        self.assertFalse(self.c.magnet_active)
        self.assertTrue(any(k == 'error' for k, v in self.events))
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')

    def test_continuous_session_renews_without_enabling_ac(self):
        self.start_magnet()
        self.advance(130)
        self.run_until_idle()
        self.assertTrue(self.c.magnet_active)
        self.assertFalse(self.c.armed)
        self.assertGreater(self.serial.writes.count('sys arm on 60'), 5)
        self.assertEqual(self.serial.writes.count('magnet on --json'), 1)

    def test_device_disarm_never_automatically_restarts(self):
        self.start_magnet()
        self.serial.armed = False
        before = len(self.serial.writes)
        self.advance(2)
        self.run_until_idle()
        self.assertNotIn('sys arm on 60', self.serial.writes[before:])
        self.assertFalse(self.c.magnet_active)

    def test_active_interlock_open_triggers_stop(self):
        self.start_magnet()
        self.serial.interlock = False
        self.advance(2)
        self.run_until_idle()
        self.assertFalse(self.c.magnet_active)
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')

    def test_ac_and_magnet_share_authorization_not_output(self):
        self.c.arm(80)
        self.run_until_idle()
        self.c.select_continuous([1])
        self.run_until_idle()
        self.serial.writes.clear()
        self.start_magnet()
        self.start_magnet('off')
        self.assertTrue(self.c.armed)
        self.assertEqual(self.serial.mode, 'ac')
        self.assertFalse(any(s.startswith(('hv ', 'array ', 'all_stop')) for s in self.serial.writes))

    def test_invalid_parameters_are_rejected_before_io(self):
        self.assertTrue(callable(getattr(self.c, 'set_magnet', None)), '磁杆控制入口尚未实现')
        for mode, duration in [('bad', 500), ('timed', 0), ('timed', 5001), ('timed', True)]:
            with self.assertRaises(ValueError):
                self.c.set_magnet(mode, duration)
        self.assertEqual(self.serial.writes, [])

    def test_sequence_wait_does_not_starve_magnet_monitor(self):
        self.c.arm(80)
        self.run_until_idle()
        self.start_magnet()
        self.c.sequence([([1], 10000)])
        self.advance(.5)
        self.serial.interlock = False
        self.advance(2)
        self.assertFalse(self.c.magnet_active)
        self.assertFalse(self.c.busy)
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')

    def test_lost_connection_reports_unknown_then_closes_without_retrying_on(self):
        self.start_magnet()
        self.serial.auto_reply = False
        self.advance(5)
        self.assertFalse(self.c.ready)
        self.assertFalse(self.c.magnet_active)
        self.assertFalse(self.serial.is_open)
        self.assertTrue(self.c.uncertain)
        self.assertEqual(self.serial.writes.count('magnet on --json'), 1)
