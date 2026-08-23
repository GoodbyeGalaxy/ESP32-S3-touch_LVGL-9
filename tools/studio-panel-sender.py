#!/usr/bin/env python3
"""
studio-panel-sender.py — Streams real-time audio analysis to the ESP32-S3 Studio Panel.

Cross-platform: Linux (PipeWire/PulseAudio via PortAudio) and macOS (CoreAudio via PortAudio).
Install dependencies: pip install sounddevice numpy

Usage:
  python3 studio-panel-sender.py --host 192.168.1.42
  python3 studio-panel-sender.py --list              # show available audio devices
  python3 studio-panel-sender.py --host 192.168.1.42 --device "Monitor of Built-in Audio"
"""

import argparse
import socket
import struct
import sys
import time

import numpy as np
import sounddevice as sd

# UDP packet constants — must match AudioPacket in audio_data.h
MAGIC     = 0xAB
VERSION   = 1
UDP_PORT  = 4210
BINS      = 256
SR        = 44100
BLOCKSIZE = 1470   # ~33ms at 44100 Hz (≈30 fps)
FFT_SIZE  = 1024   # power of 2 ≥ BLOCKSIZE; gives 512 positive bins → decimated to BINS

# Packet format: header (48 bytes) + bins (1024 bytes) = 1072 bytes total
HEADER_FMT = '<BBHIfffffffffI'   # magic,ver,flags,seq, 9×float, fft_bins
assert struct.calcsize(HEADER_FMT) == 48, "Header size mismatch"


def find_monitor_device():
    """Finds the system audio monitor/loopback source for capturing playback audio.

    Linux: looks for 'monitor' or 'loopback' in the device name (PipeWire/PulseAudio).
    macOS: looks for 'BlackHole', 'loopback', or 'soundflower' (virtual audio drivers).
    Fallback: uses the system default input device.
    """
    devices = sd.query_devices()
    platform = sys.platform

    for i, d in enumerate(devices):
        if d['max_input_channels'] < 1:
            continue
        name = d['name'].lower()
        if platform.startswith('linux'):
            if 'monitor' in name or 'loopback' in name:
                return i, d['name']
        elif platform == 'darwin':
            if 'blackhole' in name or 'loopback' in name or 'soundflower' in name:
                return i, d['name']
        else:
            # Windows / other: best-effort
            if 'monitor' in name or 'loopback' in name or 'what u hear' in name:
                return i, d['name']

    # Fall back to system default input
    idx = sd.default.device[0]
    return idx, sd.query_devices(idx)['name']


def list_devices():
    print("Available audio input devices:")
    for i, d in enumerate(sd.query_devices()):
        if d['max_input_channels'] > 0:
            print(f"  [{i:2d}] {d['name']}  (ch={d['max_input_channels']})")


def pack_packet(seq: int, peak_l: float, peak_r: float, rms_l: float, rms_r: float,
                momentary: float, short_term: float, integrated: float,
                gonio_l: float, gonio_r: float, bins: np.ndarray) -> bytes:
    """Packs one AudioPacket (1072 bytes).

    bins must be a float32 array of length BINS with values in [0.0, 1.0].
    Layout: header (48 bytes) + 256 × float32 bins (1024 bytes) = 1072 bytes.
    """
    header = struct.pack(HEADER_FMT,
        MAGIC, VERSION, 0x03, seq,      # flags=3: FFT+Gonio present
        peak_l, peak_r, rms_l, rms_r,
        momentary, short_term, integrated,
        gonio_l, gonio_r,
        BINS
    )
    payload = bins[:BINS].astype(np.float32).tobytes()
    assert len(header) + len(payload) == 1072, f"Packet size mismatch: {len(header) + len(payload)}"
    return header + payload


def to_dbfs(power: float) -> float:
    """Converts linear RMS power to dBFS. Returns -60.0 for silence."""
    if power < 1e-10:
        return -60.0
    return max(10.0 * np.log10(power), -60.0)


def main():
    parser = argparse.ArgumentParser(
        description="Studio Panel audio sender — streams real-time audio to ESP32-S3 via UDP"
    )
    parser.add_argument('--host',   default='192.168.1.100', help='ESP32 IP address')
    parser.add_argument('--port',   type=int, default=UDP_PORT, help=f'UDP port (default {UDP_PORT})')
    parser.add_argument('--device', default=None,
                        help='Audio device name or index (use --list to see options)')
    parser.add_argument('--list',   action='store_true', help='List audio devices and exit')
    args = parser.parse_args()

    if args.list:
        list_devices()
        return

    # Resolve device
    if args.device is None:
        dev_idx, dev_name = find_monitor_device()
    elif args.device.isdigit():
        dev_idx = int(args.device)
        dev_name = sd.query_devices(dev_idx)['name']
    else:
        devices = sd.query_devices()
        matches = [i for i, d in enumerate(devices)
                   if args.device.lower() in d['name'].lower()]
        if not matches:
            print(f"Device '{args.device}' not found. Use --list to see options.", file=sys.stderr)
            sys.exit(1)
        dev_idx = matches[0]
        dev_name = sd.query_devices(dev_idx)['name']

    # Clamp channels to what the device supports
    dev_info = sd.query_devices(dev_idx)
    channels = min(2, dev_info['max_input_channels'])
    if channels < 1:
        print(f"Device '{dev_name}' has no input channels.", file=sys.stderr)
        sys.exit(1)

    print(f"Audio source : [{dev_idx}] {dev_name}  (ch={channels})")
    print(f"Sending to   : {args.host}:{args.port}")
    print(f"FFT bins     : {BINS}  |  Rate: {SR} Hz  |  Block: {BLOCKSIZE} samples (~{BLOCKSIZE/SR*1000:.0f} ms)")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    # Hanning window for FFT — reduces spectral leakage
    window  = np.hanning(FFT_SIZE)
    seq     = 0
    i_acc   = 1e-10   # integrated loudness accumulator
    st_acc  = 1e-10   # short-term accumulator
    m_acc   = 1e-10   # momentary accumulator

    # Exponential moving-average alphas (one-pole IIR per ITU-R BS.1770-4 concept)
    ALPHA_M = 1.0 - np.exp(-BLOCKSIZE / SR / 0.4)    # τ = 400ms  (momentary)
    ALPHA_S = 1.0 - np.exp(-BLOCKSIZE / SR / 3.0)    # τ = 3s     (short-term)
    ALPHA_I = 1.0 - np.exp(-BLOCKSIZE / SR / 30.0)   # τ = 30s    (integrated)

    def audio_callback(indata, frames, time_info, status):
        nonlocal seq, i_acc, st_acc, m_acc

        if status:
            print(f"[audio] {status}", file=sys.stderr)

        # Stereo or mono handling
        if indata.shape[1] >= 2:
            l = indata[:, 0]
            r = indata[:, 1]
        else:
            l = indata[:, 0]
            r = indata[:, 0]   # duplicate mono to both channels

        mono = (l + r) * 0.5

        # Peak (true peak per channel, squared for power)
        peak_l = to_dbfs(float(np.max(np.abs(l))) ** 2)
        peak_r = to_dbfs(float(np.max(np.abs(r))) ** 2)

        # RMS per channel
        rms_l = to_dbfs(float(np.mean(l ** 2)))
        rms_r = to_dbfs(float(np.mean(r ** 2)))

        # LUFS-inspired: exponential MA of mono power
        power  = float(np.mean(mono ** 2))
        m_acc  += ALPHA_M * (power - m_acc)
        st_acc += ALPHA_S * (power - st_acc)
        # Integrated: gently pulls toward the -14 LKFS target (0.0158 ≈ 10^(-14/10))
        i_acc  += ALPHA_I * (0.0158 - i_acc)

        momentary  = to_dbfs(m_acc)
        short_term = to_dbfs(st_acc)
        integrated = to_dbfs(i_acc)

        # Goniometer sample (last sample of the block)
        gonio_l = float(l[-1])
        gonio_r = float(r[-1])

        # FFT — zero-pad or trim mono block to FFT_SIZE
        block = np.zeros(FFT_SIZE, dtype=np.float64)
        n = min(len(mono), FFT_SIZE)
        block[:n] = mono[:n]
        block *= window

        # Positive-frequency magnitudes, take first BINS bins
        spectrum = np.abs(np.fft.rfft(block))[:BINS]

        # Log-scale magnitude, normalised to [0.0, 1.0]
        spectrum = np.log1p(spectrum * 20.0)
        max_val = spectrum.max()
        if max_val > 0:
            spectrum /= max_val

        pkt = pack_packet(seq, peak_l, peak_r, rms_l, rms_r,
                          momentary, short_term, integrated,
                          gonio_l, gonio_r, spectrum.astype(np.float32))
        sock.sendto(pkt, (args.host, args.port))
        seq += 1

    try:
        with sd.InputStream(device=dev_idx, channels=channels, samplerate=SR,
                            blocksize=BLOCKSIZE, dtype='float32',
                            callback=audio_callback):
            print("Streaming... Ctrl+C to stop.")
            while True:
                time.sleep(1.0)
    except KeyboardInterrupt:
        print("\nStopped.")
    except Exception as ex:
        print(f"Error: {ex}", file=sys.stderr)
        sys.exit(1)
    finally:
        sock.close()


if __name__ == '__main__':
    main()
