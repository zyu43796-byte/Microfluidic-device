import importlib.util
import json
import unittest


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class FirmwareSerial:
    """Simulates the inspected CLI; writes are the observable hardware requests."""
    def __init__(self):
        self.rx = bytearray()
        self.writes = []
        self.is_open = True
        self.auto_reply = True
        self.interlock = True
        self.bypass = False
        self.armed = False
        self.logic = [0] * 16
        self.mode = 'off'
        self.hv = dict(setpoint_v=80, ac_freq_hz=1000, ac_phase_us=40, ac_drive='pwm', wiper=10, logic_dirty=False)
        self.reject = None

    @property
    def in_waiting(self):
        return len(self.rx)

    def read(self, size):
        data = bytes(self.rx[:size])
        del self.rx[:size]
        return data

    def reset_input_buffer(self):
        self.rx.clear()

    def close(self):
        self.is_open = False

    def write(self, data):
        command = data.decode('ascii').strip()
        self.writes.append(command)
        if not self.auto_reply:
            return len(data)
        if self.reject and command.startswith(self.reject):
            self.rx.extend(b'error: rejected\r\n')
            return len(data)
        words = command.split()
        reply = dict(ok=True, cmd=words[0])
        if command.startswith('sys ping'):
            reply['pong'] = True
        elif words[0] == 'all_stop':
            self.armed = False
            self.mode = 'off'
        elif words[0] == 'gpio':
            reply.update(biochip_check=self.interlock, pos_sw1=True,
                         interlock_closed=self.interlock, biochip_interlock_bypassed=self.bypass)
        elif words[0] == 'sys':
            if words[1:3] == ['arm', 'on']:
                self.armed = True
            self.rx.extend(('uptime=1000ms armed=%s rs232_verified=no\r\n'
                            'faults=none heater_online=no\r\n'
                            'hv_mode=%s hv_set=80V ac=1000Hz\r\n'
                            'chip=%s pos=ok last_stop=cli all_stop\r\n' %
                            ('yes' if self.armed else 'no', self.mode, 'ok' if self.interlock else 'open')).encode())
            return len(data)
        elif words[0] == 'array':
            if words[1] == 'set':
                self.logic = [int(x, 16) for x in words[2:18]]
            reply['logic'] = self.logic
        elif words[0] == 'hv':
            names = {'set': 'setpoint_v', 'freq': 'ac_freq_hz', 'phase': 'ac_phase_us'}
            if words[1] in names:
                self.hv[names[words[1]]] = int(words[2])
            if words[1] == 'mode':
                self.mode = words[2]
            reply.update(self.hv, mode=self.mode)
        self.rx.extend((json.dumps(reply) + '\r\n').encode())
        return len(data)


class Availability(unittest.TestCase):
    def test_adapter_exists(self):
        self.assertIsNotNone(importlib.util.find_spec('control'), 'ASCII control adapter is not implemented')


class ControlTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec('control'), 'ASCII control adapter is not implemented')
        import control
        self.m = control
        self.clock = Clock()
        self.serial = FirmwareSerial()
        self.events = []
        self.c = control.Controller(self.clock, lambda kind, data: self.events.append((kind, data)))

    def run_until_idle(self, limit=2000):
        for _ in range(limit):
            self.clock.now += .01
            self.c.tick()
            if not self.c.busy:
                return
        self.fail('controller did not become idle')

    def connect_arm(self):
        self.c.connect(self.serial)
        self.run_until_idle()
        self.assertTrue(self.c.ready)
        self.c.arm(80)
        self.run_until_idle()
        self.assertTrue(self.c.armed)

    def test_bitmap_and_reserved_ids(self):
        data = self.m.bitmap([1, 1, 60, 65, 124])
        self.assertEqual((data[0], data[7], data[8], data[15]), (1, 8, 1, 8))
        for invalid in [0, 61, 64, 125, 128, -1, True, 1.5]:
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                self.m.bitmap([invalid])

    def test_handshake_never_arms_or_drives(self):
        self.c.connect(self.serial)
        self.run_until_idle()
        self.assertTrue(self.c.ready)
        self.assertFalse(self.c.armed)
        self.assertFalse(any('acburst' in s or 'arm on' in s for s in self.serial.writes))

    def test_idle_connection_does_not_poll_until_status_is_requested(self):
        self.c.connect(self.serial)
        self.run_until_idle()
        writes_after_handshake = list(self.serial.writes)

        for _ in range(300):
            self.clock.now += .01
            self.c.tick()

        self.assertEqual(self.serial.writes, writes_after_handshake)
        self.c.request_status()
        self.run_until_idle()
        self.assertEqual(self.serial.writes[len(writes_after_handshake):],
                         ['gpio read --json', 'sys'])

    def test_armed_idle_does_not_poll_or_block_the_next_drive(self):
        self.connect_arm()
        before = list(self.serial.writes)
        for _ in range(300):
            self.clock.now += .01
            self.c.tick()
        self.assertEqual(self.serial.writes, before)
        self.assertFalse(self.c.busy)
        self.c.select_continuous([1])
        self.run_until_idle()
        self.assertEqual(self.serial.writes[len(before):], [
            'hv mode off --json', 'gpio read --json',
            'array set 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 --json',
            'gpio read --json', 'hv mode ac --json'])
        self.assertEqual(self.serial.mode, 'ac')

        writes = list(self.serial.writes)
        self.clock.now += 5
        self.c.tick()
        self.assertEqual(self.serial.writes, writes)
        self.assertTrue(self.c.output_active)

    def test_explicit_arm_uses_text_sys_and_checks_parameters(self):
        self.connect_arm()
        self.assertIn('sys arm on 60', self.serial.writes)
        self.assertNotIn('sys --json', self.serial.writes)
        self.assertIn('hv phase 40 --json', self.serial.writes)

    def test_switch_turns_ac_off_before_array_update(self):
        self.connect_arm()
        self.c.select_continuous([1])
        self.run_until_idle()
        before = len(self.serial.writes)
        self.c.select_continuous([2])
        self.run_until_idle()
        self.assertEqual(self.serial.writes[before:], [
            'hv mode off --json', 'gpio read --json',
            'array set 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 --json',
            'gpio read --json', 'hv mode ac --json'])
        self.assertEqual(self.serial.mode, 'ac')

    def test_clear_selection_turns_ac_off_and_sends_zero_bitmap(self):
        self.connect_arm()
        self.c.select_continuous([1])
        self.run_until_idle()
        before = len(self.serial.writes)
        self.c.select_continuous([])
        self.run_until_idle()
        self.assertEqual(self.serial.writes[before:], [
            'hv mode off --json', 'gpio read --json',
            'array set 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 --json'])
        self.assertEqual(self.serial.logic, [0] * 16)
        self.assertEqual(self.serial.mode, 'off')
        self.assertFalse(self.c.output_active)
        self.assertTrue(self.c.armed)

    def test_open_or_bypassed_interlock_prevents_arm(self):
        self.c.connect(self.serial)
        self.run_until_idle()
        for attr in ['interlock', 'bypass']:
            setattr(self.serial, attr, attr == 'bypass')
            self.c.arm(80)
            self.run_until_idle()
            self.assertFalse(self.c.armed)
            self.assertFalse(any('arm on' in s for s in self.serial.writes))
            self.serial.interlock = True
            self.serial.bypass = False

    def test_link_waits_for_whole_response(self):
        self.serial.auto_reply = False
        link = self.m.Link(self.serial, self.clock)
        link.send(self.m.Command('gpio read --json', 'gpio'))
        self.serial.rx.extend(b'{"ok":true,"cmd":"gpio"')
        self.assertIsNone(link.poll())
        with self.assertRaises(RuntimeError):
            link.send(self.m.Command('hv read --json', 'hv'))
        self.serial.rx.extend(b'}\r\n')
        self.assertEqual(link.poll()['cmd'], 'gpio')

    def test_stop_from_continuous_ac_sends_all_stop(self):
        self.connect_arm()
        self.c.select_continuous([1])
        self.run_until_idle()
        self.c.stop()
        self.run_until_idle()
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')
        self.assertEqual(self.serial.mode, 'off')
        self.assertFalse(self.c.armed)
        self.assertFalse(self.c.output_active)

    def test_error_stops_without_retrying_drive(self):
        self.connect_arm()
        self.serial.reject = 'array set'
        self.c.select_continuous([1])
        self.run_until_idle()
        self.assertFalse(any(s.startswith('hv mode ac') for s in self.serial.writes))
        self.assertFalse(self.c.armed)
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')

    def test_timeout_invalidates_connection(self):
        self.connect_arm()
        self.serial.auto_reply = False
        self.c.select_continuous([1])
        self.run_until_idle()
        self.assertFalse(self.c.ready)
        self.assertFalse(self.c.armed)
        self.assertTrue(any(k == 'error' for k, _ in self.events))

    def test_authorization_expires_without_automatic_rearm(self):
        self.connect_arm()
        self.c.select_continuous([1])
        self.run_until_idle()
        self.clock.now += 56
        self.c.tick()
        self.run_until_idle()
        self.assertFalse(self.c.armed)
        self.assertEqual(self.serial.mode, 'off')
        self.assertFalse(self.c.output_active)
        self.assertEqual(sum('arm on' in s for s in self.serial.writes), 1)

    def test_auto_ends_with_stop(self):
        self.connect_arm()
        self.c.sequence([([1], 100), ([], 100), ([2], 100)])
        self.run_until_idle()
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')
        self.assertFalse(self.c.armed)
        self.assertFalse(any('acburst' in s for s in self.serial.writes))
        self.assertEqual(sum(s == 'hv mode ac --json' for s in self.serial.writes), 2)

    def test_boundary_and_real_adjacency(self):
        from chip.chip_array import id_array
        self.assertEqual(self.m.move_ids([1], [], 0, -1, id_array), [2])
        self.assertEqual(self.m.move_ids([1], [], 0, 1, id_array), [8])
        with self.assertRaises(ValueError):
            self.m.move_ids([1], [], 1, 0, id_array)

    def test_disconnect_waits_for_confirmed_stop(self):
        self.connect_arm()
        self.c.stop(disconnect=True)
        self.assertTrue(self.serial.is_open)
        self.run_until_idle()
        self.assertFalse(self.serial.is_open)
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')

    def test_changed_interlock_prevents_next_continuous_selection(self):
        self.connect_arm()
        self.c.select_continuous([1])
        self.run_until_idle()
        self.serial.interlock = False
        self.c.select_continuous([2])
        self.run_until_idle()
        self.assertEqual(sum(s == 'hv mode ac --json' for s in self.serial.writes), 1)
        self.assertEqual(self.serial.mode, 'off')
        self.assertFalse(self.c.armed)

    def test_wrong_ack_fails_closed(self):
        self.serial.auto_reply = False
        link = self.m.Link(self.serial, self.clock)
        link.send(self.m.Command('array read --json', 'array'))
        self.serial.rx.extend(b'{"ok":true,"cmd":"hv"}\r\n')
        with self.assertRaises(ValueError):
            link.poll()

    def test_sequence_rejects_reserved_id_before_any_write(self):
        self.connect_arm()
        before = len(self.serial.writes)
        with self.assertRaises(ValueError):
            self.c.sequence([([1], 100), ([61], 100)])
        self.assertEqual(len(self.serial.writes), before)

    def test_parameter_readback_mismatch_cancels_authorization(self):
        self.c.connect(self.serial)
        self.run_until_idle()
        original_write = self.serial.write

        def mismatched_write(data):
            result = original_write(data)
            if data.startswith(b'hv phase'):
                self.serial.rx = self.serial.rx.replace(b'"ac_phase_us": 40', b'"ac_phase_us": 60')
            return result

        self.serial.write = mismatched_write
        self.c.arm(80)
        self.run_until_idle()
        self.assertFalse(self.c.armed)
        self.assertEqual(self.serial.writes[-1], 'all_stop --json')

    def test_truncated_sys_response_is_not_accepted_as_authorization(self):
        self.serial.auto_reply = False
        link = self.m.Link(self.serial, self.clock)
        link.send(self.m.Command('sys arm on 60', 'sys_text'))
        self.serial.rx.extend(b'uptime=1000ms armed=yes rs232_verified=no\r\n')
        self.assertIsNone(link.poll())
        self.clock.now += 2
        with self.assertRaises(TimeoutError):
            link.poll()


if __name__ == '__main__':
    unittest.main()
