#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 用法: python wifi_audio_stream.py <设备IP> [wav文件] [端口]
#   例: python wifi_audio_stream.py 192.168.1.100 music.wav
#   仅标准库; 读 wav -> 单声道 int16 44.1kHz -> 按 512 样本分块 UDP 发送(实时节奏)
import sys, socket, wave, time, array, math

SR_OUT = 44100
CHUNK = 512  # samples per UDP packet

def read_wav_mono_int16(path):
    with wave.open(path, 'rb') as w:
        nch = w.getnchannels()
        sw = w.getsampwidth()
        fr = w.getframerate()
        nf = w.getnframes()
        raw = w.readframes(nf)
    a = array.array('h')
    if sw == 2:
        a.frombytes(raw)
    elif sw == 1:                       # 8-bit unsigned
        a.fromlist([(b - 128) << 8 for b in raw])
    elif sw == 3:                       # 24-bit
        vals = []
        for i in range(0, len(raw), 3):
            v = raw[i] | (raw[i+1] << 8) | (raw[i+2] << 16)
            if v & 0x800000:
                v -= 0x1000000
            vals.append(v >> 8)
        a.fromlist(vals)
    elif sw == 4:                       # 32-bit
        b = array.array('i'); b.frombytes(raw)
        a.fromlist([x >> 16 for x in b])
    else:
        raise ValueError('unsupported sample width %d' % sw)

    if nch == 2:                        # downmix stereo -> mono
        mono = [ (a[i] + a[i+1]) // 2 for i in range(0, len(a) - (len(a) % 2), 2) ]
    elif nch == 1:
        mono = list(a)
    else:
        mono = [ sum(a[i:i+nch]) // nch for i in range(0, len(a) - (len(a) % nch), nch) ]

    if fr != SR_OUT:                    # 线性重采样到 44.1k
        ratio = SR_OUT / fr
        out_len = int(len(mono) * ratio)
        res = []
        for j in range(out_len):
            x = j / ratio
            i0 = int(x); i1 = min(i0 + 1, len(mono) - 1); t = x - i0
            res.append(int(mono[i0] * (1 - t) + mono[i1] * t))
        mono = res
    return mono

def main():
    if len(sys.argv) < 2:
        print('用法: python wifi_audio_stream.py <设备IP> [wav文件] [端口]'); sys.exit(1)
    ip = sys.argv[1]
    path = sys.argv[2] if len(sys.argv) > 2 else None
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 5004

    if path is None:
        print('未指定 wav，尝试生成 5 秒 440Hz 测试音...')
        mono = [int(20000 * math.sin(2 * math.pi * 440 * n / SR_OUT)) for n in range(SR_OUT * 5)]
    else:
        mono = read_wav_mono_int16(path)

    print('发送 %d 样本 (~%.1fs) -> udp %s:%d' % (len(mono), len(mono)/SR_OUT, ip, port))
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    n = len(mono) - (len(mono) % CHUNK)
    for off in range(0, n, CHUNK):
        pkt = array.array('h', mono[off:off+CHUNK]).tobytes()
        sock.sendto(pkt, (ip, port))
        time.sleep(CHUNK / SR_OUT)      # 实时节奏
    print('播放完毕')

if __name__ == '__main__':
    main()
