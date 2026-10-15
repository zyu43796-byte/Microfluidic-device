"""Commands and strict text acknowledgements for pwm_fx_v4_core lighting."""
import re

COLORS = {'red': (1, 0, 0), 'green': (0, 1, 0), 'blue': (0, 0, 1),
          'white': (1, 1, 1), 'yellow': (1, 1, 0), 'cyan': (0, 1, 1),
          'magenta': (1, 0, 1)}
EFFECTS = ('rainbow', 'meteor', 'chase', 'stack', 'sparkle', 'collide')
OPTIONS = [('白色常亮', 'white'), ('红色常亮', 'red'), ('绿色常亮', 'green'),
           ('蓝色常亮', 'blue'), ('黄色常亮', 'yellow'), ('青色常亮', 'cyan'),
           ('紫红常亮', 'magenta'), ('1 · 彩虹流动', 'rainbow'),
           ('2 · 流星拖尾', 'meteor'), ('3 · 多点追逐', 'chase'),
           ('4 · 逐颗累加', 'stack'), ('5 · 星空闪烁', 'sparkle'),
           ('6 · 霓虹对撞', 'collide'), ('六种灯效轮播', 'demo')]


def light_request(mode, brightness=80, interval=30):
    if (type(brightness) is not int or not 0 <= brightness <= 255 or
            type(interval) is not int or not 10 <= interval <= 1000):
        raise ValueError('亮度须为0..255，灯效帧间隔须为10..1000毫秒')
    if mode == 'off':
        return 'ledstrip off 30', r'ledstrip off count=30(?: rgb=0,0,0)?'
    if mode in COLORS:
        rgb = ','.join(str(x * brightness) for x in COLORS[mode])
        return (f'ledstrip color 30 {mode} {brightness}',
                f'ledstrip color count=30 color={mode} brightness={brightness} rgb={rgb} ms=0')
    if mode in EFFECTS or mode == 'demo':
        action = 'demo' if mode == 'demo' else 'effect ' + mode
        effect, state = ('rainbow', 'demo') if mode == 'demo' else (mode, 'running')
        return (f'ledstrip {action} 30 {brightness} {interval}',
                f'ledstrip effect={effect} mode={state} count=30 brightness={brightness} interval_ms={interval}')
    raise ValueError('未知灯光模式')


def parse_light_reply(command, line):
    words = command.split()
    if words[1] == 'off':
        _, expected = light_request('off')
    elif words[1] == 'color':
        _, expected = light_request(words[3], int(words[4]))
    elif words[1] == 'demo':
        _, expected = light_request('demo', int(words[3]), int(words[4]))
    else:
        _, expected = light_request(words[2], int(words[4]), int(words[5]))
    if not re.fullmatch(expected, line):
        raise ValueError('灯光回执与请求不一致：' + line)
    return dict(ok=True, cmd='ledstrip', text=line)
