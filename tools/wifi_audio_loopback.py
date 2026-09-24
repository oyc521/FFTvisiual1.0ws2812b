#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 抓取 Windows“正在播放的声音”(WASAPI loopback) -> 单声道 int16 44.1k -> UDP 发给 ESP32
# 用法: python wifi_audio_loopback.py <设备IP> [端口]
#   依赖: pip install pyaudiowpatch numpy
# 说明: pyaudiowpatch 支持 WASAPI 回环,能抓到系统输出的混音(QQ/网页/播放器等)。
import sys, socket, time, os
import webbrowser
import numpy as np
import pyaudiowpatch as pa

SR_OUT = 44100
CHUNK = 512  # samples per UDP packet
DISCOVER_PORT = 5005
CACHE_FILE = os.path.join(os.path.expanduser('~'), '.esp_led_ip.txt')


def _probe(ip, port=80, timeout=1.5):
    import urllib.request
    try:
        with urllib.request.urlopen('http://%s/ping' % ip, timeout=timeout):
            return True
    except Exception:
        return False


def _read_cache():
    try:
        return open(CACHE_FILE).read().strip()
    except Exception:
        return ''


def _write_cache(ip):
    try:
        open(CACHE_FILE, 'w').write(ip)
    except Exception:
        pass


def discover_ip(timeout_s=2.5):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(0.5)
    end = time.time() + timeout_s
    while time.time() < end:
        try:
            s.sendto(b'ESPLED', ('<broadcast>', DISCOVER_PORT))
            data, addr = s.recvfrom(128)
        except socket.timeout:
            continue
        parts = data.decode('utf-8', 'ignore').split()
        if len(parts) >= 2 and parts[0] == 'ESPLED':
            s.close()
            return parts[1]
    s.close()
    return None


def resolve_ip(arg_ip):
    if arg_ip:
        return arg_ip
    c = _read_cache()
    if c and _probe(c):
        print('使用上次的设备 IP:', c)
        return c
    print('正在局域网自动发现设备(若弹窗询问防火墙,请点“允许访问”)...')
    ip = discover_ip()
    if ip:
        _write_cache(ip)
        print('发现设备:', ip)
        return ip
    return None


def main():
    ip = resolve_ip(sys.argv[1] if len(sys.argv) > 1 else None)
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5004
    if not ip:
        print('未发现设备。请确认设备已上电、与电脑同一网络;或手动指定:')
        print('    python wifi_audio_loopback.py <设备IP>')
        print('    (IP 可在设备控制页 /api/status 的 ip_address 查看)')
        sys.exit(2)

    webbrowser.open('http://%s' % ip)  # 自动打开控制页

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ring = bytearray()

    with pa.PyAudio() as p:
        try:
            lo = p.get_default_wasapi_loopback()
        except Exception as e:
            print('未找到 WASAPI 回环设备:', e)
            print('可用回环设备:')
            for d in p.get_loopback_device_info_generator():
                print('  [%s] %s  %dHz' % (d['index'], d['name'], d['defaultSampleRate']))
            return

        rate = int(lo['defaultSampleRate'])
        ch = int(lo['maxInputChannels']) or 2
        print('回环设备: [%s] %s  %dHz %dch  ->  %s:%d' % (lo['index'], lo['name'], rate, ch, ip, port))
        ratio = SR_OUT / rate
        acc = np.float32(0.0)
        pos = 0.0  # 重采样小数游标

        def cb(in_data, frame_count, time_info, status):
            nonlocal ring, pos
            data = np.frombuffer(in_data, dtype=np.float32).reshape(-1, ch)
            mono = data.mean(axis=1)
            # 线性重采样到 44.1k
            n_in = len(mono)
            idx = pos
            out = []
            while idx + 1 < n_in:
                i0 = int(idx); t = idx - i0
                out.append(mono[i0] * (1 - t) + mono[i0 + 1] * t)
                idx += ratio
            pos = idx - n_in  # 保留到下一块的偏移
            if not out:
                return (None, pa.paContinue)
            s16 = (np.clip(np.array(out, dtype=np.float32), -1, 1) * 32767).astype('<i2')
            ring.extend(s16.tobytes())
            while len(ring) >= CHUNK * 2:
                sock.sendto(bytes(ring[:CHUNK*2]), (ip, port))
                del ring[:CHUNK*2]
            return (None, pa.paContinue)

        stream = p.open(format=pa.paFloat32, channels=ch, rate=rate,
                        input=True, input_device_index=lo['index'],
                        frames_per_buffer=1024, stream_callback=cb)
        stream.start_stream()
        print('正在推流(CTRL+C 停止)...')
        try:
            while stream.is_active():
                time.sleep(0.2)
        except KeyboardInterrupt:
            pass
        finally:
            stream.stop_stream()

if __name__ == '__main__':
    main()
