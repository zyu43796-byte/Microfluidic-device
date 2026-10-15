"""Magnet relay workflows. Acknowledgments are not mechanical position feedback."""


class MagnetControl:
    def set_magnet(self, mode, duration_ms=500):
        if not self.ready:
            raise ValueError('请先连接并完成握手')
        if mode not in ('on', 'off', 'timed'):
            raise ValueError('磁杆模式须为 on / off / timed')
        if mode == 'timed' and (type(duration_ms) is not int or not 100 <= duration_ms <= 5000):
            raise ValueError('定时动作范围为 100–5000 ms')
        self._start(self._magnet(mode, duration_ms), '磁杆控制')

    def _magnet_monitor(self):
        from control import Command
        self._check_session()
        yield from self._gpio(quiet=True)
        status = yield Command('sys', 'sys_text', quiet=True)
        self._expect(status, armed=True, hv_mode=self.output_state)
        yield from self._maintain_ac_session()
        self.magnet_check_at = self.clock() + 1

    def _magnet_off(self):
        from control import Command
        was_active = self.magnet_active
        reply = yield Command('magnet off --json', 'magnet')
        self._expect(reply, state='off')
        self.magnet_active = False
        self.emit('magnet', 'off')
        if was_active and not self.armed:
            # Firmware disarm also resets lights. Do not leave a hidden live lease.
            yield Command('all_stop --json', 'all_stop')
            self.arm_deadline = self.renew_at = 0
            self.output_state = 'off'
            self.ack_ids = []
            self.emit('stopped', '磁杆已返回默认指令状态；独立会话结束，灯带同时关闭。')

    def _magnet(self, mode, duration_ms):
        from control import Command
        if mode == 'off':
            yield from self._magnet_off()
            return
        yield from self._gpio()
        if self.armed or self.magnet_active:
            yield from self._magnet_monitor()
        else:
            status = yield Command('sys', 'sys_text')
            self._expect(status, armed=False, hv_mode='off')
            started = self.clock()
            status = yield Command('sys arm on 60', 'sys_text')
            self._expect(status, armed=True, hv_mode='off')
            self.arm_deadline = started + 55
            self.renew_at = started + 20
        yield from self._gpio()
        sent_at = self.clock()
        reply = yield Command('magnet on --json', 'magnet')
        self._expect(reply, state='on')
        self.magnet_active = True
        self.magnet_check_at = self.clock() + 1
        self.emit('magnet', 'on')
        if mode == 'timed':
            yield from self._magnet_wait(sent_at + duration_ms / 1000)
            yield from self._magnet_off()

    def _magnet_wait(self, until):
        """Absolute host deadline, with interlock service during long AC frames."""
        from control import Wait
        while self.clock() < until:
            if self.magnet_active and self.clock() >= self.magnet_check_at:
                yield from self._magnet_monitor()
            remaining = until - self.clock()
            if remaining > 0:
                yield Wait(min(.1, remaining))
