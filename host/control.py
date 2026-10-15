"""Serialized, nonblocking ASCII control for Drop_Drive app_cli (no Qt dependency)."""
from dataclasses import dataclass
import json
import re
import time
from lighting import light_request, parse_light_reply
from magnet_control import MagnetControl

VALID_IDS = frozenset(range(1, 61)) | frozenset(range(65, 125))


def bitmap(ids):
    data = [0] * 16
    for electrode in ids:
        if type(electrode) is not int or electrode not in VALID_IDS:
            raise ValueError('电极编号必须为 1..60 或 65..124 的整数')
        data[(electrode - 1) // 8] |= 1 << ((electrode - 1) % 8)
    return data


def move_ids(ids, pinned, dr, dc, grid):
    bitmap(ids)
    bitmap(pinned)
    positions = {n: (r, c) for r, row in enumerate(grid) for c, n in enumerate(row) if n}
    result = set(pinned)
    for n in set(ids) - set(pinned):
        r, c = positions[n]
        r, c = r + dr, c + dc
        if not (0 <= r < len(grid) and 0 <= c < len(grid[r])) or not grid[r][c]:
            raise ValueError(f'电极 {n} 在该方向没有相邻电极；保持原选择')
        result.add(grid[r][c])
    return sorted(result)


@dataclass
class Command:
    text: str
    response: str
    timeout: float = 1.5
    quiet: bool = False


@dataclass
class Wait:
    seconds: float


class Link:
    def __init__(self, serial_port, clock=time.monotonic, emit=lambda *args: None):
        self.serial = serial_port
        self.clock = clock
        self.emit = emit
        self.pending = None
        self.buffer = bytearray()
        self.lines = []
        self.deadline = 0
        self.next_send = 0

    def send(self, command):
        if self.pending is not None or self.clock() < self.next_send:
            raise RuntimeError('上一条指令尚未完成')
        if '\n' in command.text or '\r' in command.text:
            raise ValueError('只允许单行指令')
        self.buffer.clear()
        self.lines.clear()
        self.serial.reset_input_buffer()
        self.pending = command
        self.deadline = self.clock() + command.timeout
        payload = (command.text + '\n').encode('ascii')
        if self.serial.write(payload) != len(payload):
            raise OSError('串口写入不完整')
        if not command.quiet:
            self.emit('tx', command.text)

    def poll(self):
        if self.pending is None:
            return None
        if not self.serial.is_open:
            raise OSError('串口已断开')
        self.buffer.extend(self.serial.read(min(self.serial.in_waiting, 4096)))
        if len(self.buffer) > 8192:
            raise ValueError('串口应答过长')
        while b'\n' in self.buffer:
            raw, _, remaining = self.buffer.partition(b'\n')
            self.buffer = bytearray(remaining)
            line = raw.decode('utf-8', errors='replace').strip()
            if not line:
                continue
            if not self.pending.quiet:
                self.emit('rx', line)
            if line.lower().startswith('error:'):
                raise ValueError(line)
            result = None
            if self.pending.response == 'sys_text':
                self.lines.append(line)
                if line.startswith('chip='):
                    result = self._system_status()
            elif self.pending.response == 'ledstrip_text':
                if line.startswith('ledstrip '):
                    result = parse_light_reply(self.pending.text, line)
            elif line.startswith('{'):
                result = json.loads(line)
                if result.get('ok') is not True or result.get('cmd') != self.pending.response:
                    raise ValueError('串口应答类型不匹配')
            if result is not None:
                self.pending = None
                self.next_send = self.clock() + .03
                return result
            if self.pending.response != 'sys_text':
                self.lines.append(line)
        if self.clock() >= self.deadline:
            raise TimeoutError('串口应答超时，硬件停止状态未确认')
        return None

    def _system_status(self):
        patterns = [r'uptime=(\d+)ms armed=(yes|no) rs232_verified=(yes|no)',
                    r'faults=(\S+) heater_online=(yes|no)',
                    r'hv_mode=(\S+) hv_set=(\d+)V ac=(\d+)Hz',
                    r'chip=(ok|open) pos=(ok|open) last_stop=(.*)']
        matches = []
        for pattern in patterns:
            found = [re.fullmatch(pattern, line) for line in self.lines]
            found = [match for match in found if match]
            if len(found) != 1:
                raise ValueError('系统状态应答不完整')
            matches.append(found[0])
        return dict(cmd='sys', ok=True, armed=matches[0][2] == 'yes',
                    faults=matches[1][1], hv_mode=matches[2][1],
                    biochip_check=matches[3][1] == 'ok', pos_sw1=matches[3][2] == 'ok',
                    last_stop_reason=matches[3][3])


class Controller(MagnetControl):
    def __init__(self, clock=time.monotonic, emit=lambda *args: None):
        self.clock = clock
        self.emit = emit
        self.link = None
        self.ready = False
        self.armed = False
        self.magnet_active = False
        self.magnet_check_at = 0
        self.arm_deadline = 0
        self.renew_at = 0
        self.job = None
        self.job_name = ''
        self.wait_until = 0
        self.stop_pending = False
        self.close_after_stop = False
        self.uncertain = False
        self.last_gpio = {}
        self.ack_ids = []
        self.output_state = 'unknown'
        self._reply = None
        self._queued = None

    @property
    def output_active(self):
        return self.output_state == 'ac'

    @property
    def busy(self):
        return self.job is not None or self.stop_pending or bool(self.link and self.link.pending)

    def connect(self, serial_port):
        if self.link and self.link.serial.is_open:
            raise RuntimeError('请先关闭当前串口')
        self.link = Link(serial_port, self.clock, self.emit)
        self.ready = self.armed = self.uncertain = False
        self.magnet_active = False
        self.output_state = 'unknown'
        self.stop_pending = self.close_after_stop = False
        self._start(self._handshake(), '连接检查')

    def _start(self, generator, name):
        if self.busy:
            raise RuntimeError('正在处理上一项操作，请等待应答')
        self.job = generator
        self.job_name = name
        self._reply = self._queued = None
        self.wait_until = 0
        self.emit('busy', name)

    def _handshake(self):
        reply = yield Command('sys ping --json', 'sys')
        if reply.get('pong') is not True:
            raise ValueError('不是受支持的 CLI 固件')
        yield Command('all_stop --json', 'all_stop')
        self.output_state = 'off'
        yield from self._gpio(False)
        hv = yield Command('hv read --json', 'hv')
        self._expect(hv, mode='off', ac_drive='pwm')
        if not all(k in hv for k in ('ac_phase_us', 'ac_freq_hz', 'setpoint_v')):
            raise ValueError('固件缺少当前版本的 HV 状态字段')
        status = yield Command('sys', 'sys_text')
        if status['armed']:
            raise ValueError('连接初始化未解除授权')
        self.ready = True
        self.ack_ids = []
        self.emit('connected', status)

    @staticmethod
    def _expect(reply, **expected):
        for key, value in expected.items():
            actual = reply.get(key)
            if isinstance(actual, str):
                actual = actual.lower()
            if actual != value:
                raise ValueError(f'回读不一致：{key} 期望 {value}，实际 {reply.get(key)}')

    def _gpio(self, require_closed=True, quiet=False):
        reply = yield Command('gpio read --json', 'gpio', quiet=quiet)
        keys = ('biochip_check', 'pos_sw1', 'interlock_closed', 'biochip_interlock_bypassed')
        if any(type(reply.get(k)) is not bool for k in keys):
            raise ValueError('联锁应答字段缺失或类型错误')
        self.last_gpio = reply
        self.emit('gpio', reply)
        if require_closed and (reply['biochip_interlock_bypassed'] or
                               not all(reply[k] for k in keys[:3])):
            raise ValueError('压实/位置联锁未闭合，或固件启用了联锁旁路')
        return reply

    def arm(self, voltage=80):
        if not self.ready:
            raise ValueError('请先连接并完成握手')
        if type(voltage) is not int or not 40 <= voltage <= 150:
            raise ValueError('首版调试电压范围为 40..150V')
        self._start(self._arm(voltage), '开启 AC 模式并应用参数')

    def _arm(self, voltage):
        yield from self._gpio()
        started = self.clock()
        status = yield Command('sys arm on 60', 'sys_text')
        if status.get('armed') is not True:
            raise ValueError('固件未确认授权')
        reply = yield Command('hv mode off --json', 'hv')
        self._expect(reply, mode='off')
        yield from self._set_array([])
        for text, expected in [('hv set ' + str(voltage), {'setpoint_v': voltage}),
                               ('hv freq 1000', {'ac_freq_hz': 1000}),
                               ('hv phase 40', {'ac_phase_us': 40}),
                               ('hv acdrive pwm', {'ac_drive': 'pwm'})]:
            reply = yield Command(text + ' --json', 'hv')
            self._expect(reply, **expected)
        yield from self._gpio()
        self.arm_deadline = started + 55
        self.renew_at = started + 20
        self.armed = True
        self.output_state = 'off'
        self.emit('armed', voltage)

    def _check_armed(self):
        if not self.ready or not self.armed or self.clock() >= self.arm_deadline:
            raise ValueError('AC 模式未开启或通信保持已失效，请重新点击开启 AC 模式')

    def _maintain_ac_session(self):
        """Renew only a live, interlocked session; never restart a stopped device."""
        self._check_session()
        if self.clock() < self.renew_at:
            return
        yield from self._gpio(quiet=True)
        status = yield Command('sys', 'sys_text', quiet=True)
        if not status['armed'] or status['hv_mode'] != self.output_state:
            raise ValueError('固件运行状态已改变，停止保持 AC；请检查后手动重新开启')
        self._check_session()
        started = self.clock()
        status = yield Command('sys arm on 60', 'sys_text', quiet=True)
        if not status['armed'] or status['hv_mode'] != self.output_state:
            raise ValueError('固件未确认 AC 通信保持状态')
        yield from self._gpio(quiet=True)
        self.arm_deadline = started + 55
        self.renew_at = started + 20

    def _check_session(self):
        if not self.ready or not (self.armed or self.magnet_active) or self.clock() >= self.arm_deadline:
            raise ValueError('输出会话未开启或通信保持已失效，请检查后手动重新开启')

    def set_light(self, mode, brightness=80, interval=30):
        if not self.ready:
            raise ValueError('请先连接并完成握手')
        command, _ = light_request(mode, brightness, interval)
        self._start(self._set_light(command), '灯光设置')

    def _set_light(self, command):
        if self.armed or self.magnet_active:
            yield from self._maintain_ac_session()
        reply = yield Command(command, 'ledstrip_text')
        self.emit('light', reply['text'])

    def _set_array(self, ids):
        data = bitmap(ids)
        reply = yield Command('array set ' + ' '.join(f'{x:02X}' for x in data) + ' --json', 'array')
        self._expect(reply, logic=data)
        self.ack_ids = sorted(set(ids))
        self.emit('array', self.ack_ids)

    @staticmethod
    def validate_frame(ids, duration_ms):
        bitmap(ids)
        if type(duration_ms) is not int or not 1 <= duration_ms <= 10000:
            raise ValueError('每帧连续驱动时间须在 0.001..10 秒')

    def select_continuous(self, ids):
        ids = list(ids)
        bitmap(ids)
        self._check_armed()
        self._start(self._select_frame(ids), '连续 AC 电极切换')

    def _select_frame(self, ids):
        yield from self._maintain_ac_session()
        reply = yield Command('hv mode off --json', 'hv')
        self._expect(reply, mode='off')
        self.output_state = 'off'
        yield from self._gpio()
        yield from self._set_array(ids)
        if not ids:
            self.emit('continuous_off', [])
            return
        yield from self._gpio()
        self._check_armed()
        reply = yield Command('hv mode ac --json', 'hv')
        self._expect(reply, mode='ac')
        self.output_state = 'ac'
        self.emit('continuous_on', self.ack_ids)

    def sequence(self, rows):
        rows = [(list(ids), duration) for ids, duration in rows]
        if not rows or len(rows) > 1000:
            raise ValueError('流程须包含 1..1000 帧')
        for ids, duration in rows:
            self.validate_frame(ids, duration)
        self._check_armed()
        self._start(self._sequence(rows), '自动流程')

    def _sequence(self, rows):
        for index, (ids, duration) in enumerate(rows):
            self.emit('row', index)
            yield from self._select_frame(ids)
            if self.clock() + duration / 1000 >= self.arm_deadline:
                raise ValueError('通信保持期限不足，流程停止')
            if self.magnet_active:
                yield from self._magnet_wait(self.clock() + duration / 1000)
            else:
                yield Wait(duration / 1000)
        yield Command('all_stop --json', 'all_stop')
        self.armed = False
        self.magnet_active = False
        self.output_state = 'off'
        self.ack_ids = []
        self.emit('stopped', '流程完成，停止已确认')

    def stop(self, disconnect=False):
        self.armed = False
        self.magnet_active = False
        self.arm_deadline = 0
        self.job = None
        self._queued = self._reply = None
        self.wait_until = 0
        self.close_after_stop |= disconnect
        if self.output_state != 'off':
            self.output_state = 'unknown'
        if self.link and self.link.serial.is_open:
            self.stop_pending = True
            self.emit('stopping', '正在发送停止并等待固件确认')
        else:
            self.ready = False
            self.emit('disconnected', '未连接；无法确认硬件输出')

    def _stop_job(self):
        yield Command('all_stop --json', 'all_stop')
        self.output_state = 'off'
        self.ack_ids = []
        self.emit('stopped', '停止已确认；板4升压电源仍需手动断电')
        if self.close_after_stop or self.uncertain:
            self._close()

    def _close(self):
        if self.link:
            self.link.serial.close()
        self.ready = self.armed = False
        self.magnet_active = False
        self.output_state = 'unknown'
        self.close_after_stop = False
        self.emit('disconnected', '串口已关闭')

    def request_status(self):
        if not self.ready:
            raise ValueError('请先连接并完成握手')
        self._start(self._monitor(), '状态查询')

    def _monitor(self):
        yield from self._gpio(self.armed or self.magnet_active)
        status = yield Command('sys', 'sys_text')
        self.emit('system', status)
        if (self.armed or self.magnet_active) and not status['armed']:
            raise ValueError('固件已取消授权：' + status['last_stop_reason'])

    def tick(self):
        if not self.link:
            return
        try:
            if (self.armed or self.magnet_active) and self.clock() >= self.arm_deadline:
                self.emit('error', '通信保持已超时，停止输出；需手动重新开启 AC 模式')
                self.stop()
            reply = self.link.poll()
            if reply is not None:
                self._reply = reply
            if self.link.pending:
                return
            if self.stop_pending:
                self.stop_pending = False
                self.job = self._stop_job()
                self.job_name = '停止'
                self._reply = self._queued = None
            if self.clock() < max(self.wait_until, self.link.next_send):
                return
            if self.job is None:
                if self.magnet_active and self.clock() >= self.magnet_check_at:
                    self.job = self._magnet_monitor()
                    self.job_name = '磁杆状态检查'
                    self._reply = self._queued = None
                elif (self.armed or self.magnet_active) and self.clock() >= self.renew_at:
                    self.job = self._maintain_ac_session()
                    self.job_name = 'AC 通信保持'
                    self._reply = self._queued = None
                else:
                    return
            if self._queued is None:
                try:
                    self._queued = self.job.send(self._reply)
                    self._reply = None
                except StopIteration:
                    name = self.job_name
                    self.job = None
                    self.emit('idle', name)
                    return
            if isinstance(self._queued, Wait):
                self.wait_until = self.clock() + self._queued.seconds
            else:
                self.link.send(self._queued)
            self._queued = None
        except (OSError, ValueError, RuntimeError, TimeoutError) as exc:
            self.emit('error', str(exc))
            stopping_failed = self.job_name == '停止'
            self.armed = False
            self.magnet_active = False
            self.arm_deadline = 0
            self.output_state = 'unknown'
            self.job = None
            self._queued = self._reply = None
            self.link.pending = None
            self.link.next_send = self.clock() + .05
            self.wait_until = 0
            if isinstance(exc, (OSError, TimeoutError)) or not self.ready:
                self.ready = False
                self.uncertain = True
            if stopping_failed:
                self.stop_pending = False
                self.emit('error', '停止未得到确认，请手动切断高压供电')
                self._close()
            else:
                self.stop_pending = True
