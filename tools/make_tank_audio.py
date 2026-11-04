"""生成 samples/tank 的 8-bit 音效与循环 BGM（确定性、无外部素材）。

  * 全部用方波/三角波/噪声合成，22050Hz / 16-bit / mono WAV；
  * 音量刻意压低（BGM 0.16、音效 0.3~0.55），避免刺耳；
  * 只写 samples/tank/audio；重跑逐字节确定。
  * 曲子是原创的进行曲片段（不是 FC 原曲采样）。

用法：python tools/make_tank_audio.py
"""
from __future__ import annotations

import math
import os
import random
import struct
import wave

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "samples", "tank", "audio")
RATE = 22050


def midi(n: int) -> float:
    return 440.0 * (2.0 ** ((n - 69) / 12.0))


def square(phase: float) -> float:
    return 1.0 if phase % 1.0 < 0.5 else -1.0


def triangle(phase: float) -> float:
    p = phase % 1.0
    return 4.0 * abs(p - 0.5) - 1.0


def env(t: float, dur: float, attack: float = 0.004, decay: float = 0.03) -> float:
    if t < attack:
        return t / attack
    rel = min(1.0, max(0.0, (dur - t) / max(0.001, decay)))
    return rel


def write(name: str, samples: list[float]) -> None:
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name)
    peak = max(1e-9, max(abs(s) for s in samples))
    data = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s / peak * 0.92)) * 32767))
                    for s in samples)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data)
    print(f"  {name:20s} {len(samples) / RATE:5.2f}s  {os.path.getsize(path)} bytes")


def melody(notes, beat: float, vol: float = 0.5, wave_fn=square) -> list[float]:
    out: list[float] = []
    for n, beats in notes:
        length = int(RATE * beat * beats)
        for i in range(length):
            t = i / RATE
            amp = env(t, length / RATE, 0.003, min(0.04, beat * 0.3))
            out.append(wave_fn(midi(n) * t) * vol * amp)
    return out


def noise_burst(dur: float, vol: float = 0.4, seed: int = 1, low: bool = False) -> list[float]:
    rng = random.Random(seed)
    out = []
    last = 0.0
    for i in range(int(RATE * dur)):
        t = i / RATE
        v = rng.uniform(-1.0, 1.0)
        if low:
            last = last * 0.86 + v * 0.14
            v = last
        out.append(v * vol * (1.0 - t / dur) ** 1.8)
    return out


def mix(*tracks: list[float]) -> list[float]:
    n = max(len(t) for t in tracks)
    out = [0.0] * n
    for tr in tracks:
        for i, v in enumerate(tr):
            out[i] += v
    return out


def make_sfx() -> None:
    write("fire.wav", melody([(96, 0.25), (84, 0.25), (72, 0.5)], 0.10, 0.45))
    write("brick.wav", noise_burst(0.10, 0.55, seed=11, low=True))
    write("steel.wav", mix(melody([(108, 0.2), (96, 0.4)], 0.06, 0.30),
                           noise_burst(0.05, 0.15, seed=12)))
    write("hit.wav", noise_burst(0.06, 0.35, seed=13))
    write("explode.wav", mix(noise_burst(0.36, 0.55, seed=14, low=True),
                             melody([(48, 1.0), (36, 1.0)], 0.09, 0.18, triangle)))
    write("player_hit.wav", mix(melody([(72, 0.5), (60, 0.5), (48, 1.0), (36, 1.5)], 0.14, 0.40),
                                noise_burst(0.12, 0.35, seed=15, low=True)))
    write("powerup.wav", melody([(72, 0.5), (76, 0.5), (79, 0.5), (84, 1.0), (88, 1.0)],
                                0.07, 0.42))
    write("stage.wav", melody([(60, 0.5), (64, 0.5), (67, 0.5), (72, 0.5),
                               (71, 0.5), (67, 0.5), (64, 1.5)], 0.12, 0.42))
    write("stage_clear.wav", melody([(72, 0.5), (76, 0.5), (79, 0.5), (84, 0.5),
                                     (79, 0.5), (84, 1.0), (88, 1.5)], 0.10, 0.45))
    write("game_over.wav", mix(melody([(60, 0.5), (55, 0.5), (52, 0.5), (48, 0.5),
                                       (43, 2.0)], 0.18, 0.45),
                               noise_burst(0.8, 0.16, seed=16, low=True)))
    write("pause.wav", melody([(84, 0.3), (76, 0.3)], 0.08, 0.38))


def make_bgm() -> None:
    # 原创 8 秒循环：D 小调进行，方波旋律 + 三角波低音 + 稀疏军鼓。
    beat = 0.125  # 8 分音符
    lead = [
        (62, 1), (69, 1), (74, 1), (69, 1), (77, 1), (74, 1), (69, 1), (65, 1),
        (67, 1), (74, 1), (79, 1), (74, 1), (71, 1), (67, 1), (62, 1), (64, 1),
        (62, 1), (69, 1), (74, 1), (81, 1), (79, 1), (74, 1), (69, 1), (67, 1),
        (65, 1), (72, 1), (77, 1), (81, 1), (79, 1), (74, 1), (69, 2),
    ]
    bass_roots = [38, 38, 34, 34, 36, 36, 38, 38, 38, 38, 33, 33, 35, 35, 38, 38,
                  38, 38, 34, 34, 36, 36, 38, 38, 31, 31, 36, 36, 38, 38, 38, 38]
    lead_tr = melody(lead, beat, 0.20)
    bass = []
    for root in bass_roots:
        # 每拍 4 个 16 分柱式低音
        for k in range(4):
            n = root + (7 if k == 2 else 0)
            length = int(RATE * beat / 4)
            for i in range(length):
                t = i / RATE
                bass.append(triangle(midi(n) * t) * 0.16 * env(t, length / RATE, 0.002, 0.02))
    # 军鼓：每拍 2、4 反拍
    drums = []
    rng = random.Random(1990)
    total = len(lead_tr)
    for i in range(total):
        t = i / RATE
        beat_idx = int(t / (beat * 2))
        # 每个 8 分音符位置放一点噪声（弱）
        drums.append(0.0)
    for i in range(0, total, int(RATE * beat * 2)):
        for j in range(int(RATE * 0.035)):
            if i + j >= total:
                break
            drums[i + j] += rng.uniform(-1, 1) * 0.09 * (1.0 - j / (RATE * 0.035))
    loop = mix(lead_tr, bass, drums)
    # 首尾 20ms 淡入淡出，循环点不爆音
    fade = int(RATE * 0.02)
    for i in range(fade):
        loop[i] *= i / fade
        loop[-1 - i] *= i / fade
    write("battle_loop.wav", loop)


def main() -> None:
    make_sfx()
    make_bgm()
    print(f"tank audio -> {OUT}")


if __name__ == "__main__":
    main()
