#!/usr/bin/env python3
"""
studio-panel-sender.py — Streams real-time audio analysis to the ESP32-S3 Studio Panel.

Cross-platform: Linux (PipeWire/PulseAudio via PortAudio) and macOS (CoreAudio via PortAudio).
Install dependencies: pip install sounddevice numpy websockets

Usage:
  python3 studio-panel-sender.py --host 192.168.1.42
  python3 studio-panel-sender.py --list              # show available audio devices
  python3 studio-panel-sender.py --host 192.168.1.42 --device "Monitor of Built-in Audio"
  python3 studio-panel-sender.py --host 192.168.1.42 --ws-port 4211
"""

import argparse
import asyncio
import json
import socket
import struct
import sys
import threading
import time

import numpy as np
import sounddevice as sd

# UDP packet constants — must match AudioPacket in audio_data.h
MAGIC     = 0xAB
VERSION   = 1
UDP_PORT  = 4210
WS_PORT   = 4211
BINS      = 256
SR        = 44100
BLOCKSIZE = 1470   # ~33ms at 44100 Hz (≈30 fps)
FFT_SIZE  = 1024   # power of 2 ≥ BLOCKSIZE; gives 512 positive bins → decimated to BINS

# Packet format: header (56 bytes) + bins (1024 bytes) = 1080 bytes total
HEADER_FMT = '<BBHIfffffffffffI'   # magic,ver,flags,seq, 11×float, fft_bins
assert struct.calcsize(HEADER_FMT) == 56, "Header size mismatch"

# ── Onset detection state ─────────────────────────────────────────────────────

# Spectral flux threshold: running_mean × factor triggers an onset
_ONSET_ALPHA      = 0.05   # IIR alpha for adaptive threshold (≈650ms half-life at 30Hz)
_ONSET_FACTOR     = 2.5    # how many × above mean counts as a hit
_ONSET_REFRACTORY = 3      # minimum frames between hits per instrument (~100ms at 30Hz)

# Bin ranges (out of 256 log-scaled bins ≈ 20Hz–20kHz)
# These overlap intentionally — snare has body (tom range) and crack (hat range)
_ONSET_BANDS = {
    'kick':  (0,  5),    # ~20–430 Hz   — sub and kick body
    'tom':   (5,  15),   # ~430–1290 Hz — tom body
    'snare': (10, 40),   # ~860–3440 Hz — snare body + crack
    'hat':   (80, 256),  # ~6.9–22 kHz  — hi-hat and cymbals
}

# Per-instrument state: [running_mean_flux, cooldown_frames_remaining]
_onset_state = {k: [0.0, 0] for k in _ONSET_BANDS}
_prev_mag    = np.zeros(BINS, dtype=np.float64)
_prev_rms    = 1e-10

# ── WebSocket server state ────────────────────────────────────────────────────

_ws_loop:  asyncio.AbstractEventLoop | None = None
_ws_queue: asyncio.Queue | None = None
_ws_clients: set = set()

# ── Onset / analysis functions ────────────────────────────────────────────────

def _detect_onsets(fft_mag_256: np.ndarray) -> dict[str, bool]:
    """Flux-based per-band onset detection with adaptive threshold and refractory period.

    IN: fft_mag_256 — raw (pre-log) magnitude array, length 256, values ≥ 0.
    OUT: dict with bool per instrument key.
    Updates module-level _onset_state and _prev_mag in place.
    """
    global _prev_mag

    flux = np.maximum(0.0, fft_mag_256 - _prev_mag)
    _prev_mag = fft_mag_256.copy()

    hits = {}
    for inst, (lo, hi) in _ONSET_BANDS.items():
        st = _onset_state[inst]
        band_flux = float(flux[lo:hi].sum())

        # Adaptive threshold
        st[0] += _ONSET_ALPHA * (band_flux - st[0])
        threshold = st[0] * _ONSET_FACTOR

        if st[1] > 0:
            # Still in refractory window — can't trigger
            st[1] -= 1
            hits[inst] = False
        elif band_flux > threshold and st[0] > 1e-6:
            hits[inst] = True
            st[1] = _ONSET_REFRACTORY
        else:
            hits[inst] = False

    return hits


def _compute_analysis(fft_mag_raw: np.ndarray,
                      rms_l_lin: float,
                      rms_r_lin: float) -> dict:
    """Computes all analysis features from one audio frame.

    IN: fft_mag_raw — pre-log magnitude array length 256; rms_l/r_lin — linear RMS power.
    OUT: dict ready to JSON-serialise and send as type="analysis" WebSocket message.
    """
    global _prev_rms

    # Band energies — normalised so the loudest band = 1.0
    eb = float(fft_mag_raw[0:20].mean())
    em = float(fft_mag_raw[20:100].mean())
    eh = float(fft_mag_raw[100:256].mean())
    band_max = max(eb, em, eh, 1e-9)
    energy_bass = eb / band_max
    energy_mid  = em / band_max
    energy_high = eh / band_max

    # Spectral centroid: weighted mean frequency bin, normalised 0..1
    weights = fft_mag_raw + 1e-9
    centroid = float(np.average(np.arange(BINS), weights=weights) / BINS)
    centroid = min(1.0, centroid * 4.0)   # stretch — music rarely fills top octaves

    # Spectral flatness: geometric mean / arithmetic mean (0=tonal, 1=noise)
    log_mean   = float(np.mean(np.log(fft_mag_raw + 1e-9)))
    arith_mean = float(fft_mag_raw.mean()) + 1e-9
    flatness   = float(min(1.0, np.exp(log_mean) / arith_mean))

    # Onset detection
    hits = _detect_onsets(fft_mag_raw)

    # Loudness delta (signed, clipped to ±1)
    rms_now    = (rms_l_lin + rms_r_lin) * 0.5
    loudness_delta = float(np.clip(rms_now - _prev_rms, -1.0, 1.0))
    _prev_rms  = rms_now

    return {
        'type':             'analysis',
        'kick':             hits['kick'],
        'snare':            hits['snare'],
        'hat':              hits['hat'],
        'tom':              hits['tom'],
        'energy_bass':      round(energy_bass, 3),
        'energy_mid':       round(energy_mid,  3),
        'energy_high':      round(energy_high, 3),
        'spectral_centroid': round(centroid,   3),
        'spectral_flatness': round(flatness,   3),
        'loudness_delta':    round(loudness_delta, 3),
    }


# ── WebSocket server ──────────────────────────────────────────────────────────

_TRANSPORT_STUB = json.dumps({
    'type': 'transport', 'bpm': 0, 'timesig': '4/4',
    'pos': '---', 'state': 'stopped',
})

async def _ws_handler(websocket):
    """Accepts one WebSocket client on /studio-one, registers it for broadcasts."""
    path = getattr(websocket, 'path', getattr(websocket, 'request', None))
    # websockets ≥11 exposes path via websocket.request.path
    _ws_clients.add(websocket)
    try:
        await websocket.send(_TRANSPORT_STUB)
        await websocket.wait_closed()
    finally:
        _ws_clients.discard(websocket)


async def _ws_broadcast_loop(queue: asyncio.Queue):
    """Reads analysis dicts from queue and broadcasts JSON to all connected clients."""
    while True:
        msg = await queue.get()
        if not _ws_clients:
            continue
        data = json.dumps(msg)
        dead = set()
        for ws in list(_ws_clients):
            try:
                await ws.send(data)
            except Exception:
                dead.add(ws)
        _ws_clients -= dead


async def _run_ws_server(host: str, port: int, queue: asyncio.Queue):
    """Starts the WebSocket server and broadcast loop."""
    try:
        import websockets
    except ImportError:
        print("Warning: 'websockets' not installed — WebSocket server disabled. "
              "UDP audio sender still active. (pip install websockets)", file=sys.stderr)
        # Keep loop alive so call_soon_threadsafe still works safely
        while True:
            await asyncio.sleep(3600)

    async with websockets.serve(_ws_handler, host, port):
        await _ws_broadcast_loop(queue)


def _ws_thread_main(host: str, port: int):
    """Entry point for the WebSocket daemon thread."""
    global _ws_loop, _ws_queue
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    queue = asyncio.Queue()
    _ws_loop  = loop
    _ws_queue = queue
    try:
        loop.run_until_complete(_run_ws_server(host, port, queue))
    finally:
        loop.close()


# ── Helpers ───────────────────────────────────────────────────────────────────

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
            if 'monitor' in name or 'loopback' in name or 'what u hear' in name:
                return i, d['name']

    idx = sd.default.device[0]
    return idx, sd.query_devices(idx)['name']


def list_devices():
    print("Available audio input devices:")
    for i, d in enumerate(sd.query_devices()):
        if d['max_input_channels'] > 0:
            print(f"  [{i:2d}] {d['name']}  (ch={d['max_input_channels']})")


def pack_packet(seq: int, peak_l: float, peak_r: float, rms_l: float, rms_r: float,
                rms_mono: float, rms_side: float,
                momentary: float, short_term: float, integrated: float,
                gonio_l: float, gonio_r: float, bins: np.ndarray,
                fft_bins: int = BINS) -> bytes:
    """Packs one AudioPacket (1080 bytes).

    bins must be a float32 array of length fft_bins with values in [0.0, 1.0].
    Layout: header (56 bytes) + 256 × float32 bins (1024 bytes) = 1080 bytes.
    rms_mono = RMS of (L+R)/2 — true phase-correct mono sum.
    rms_side = RMS of (L-R)/2 — true phase-correct side signal.
    """
    header = struct.pack(HEADER_FMT,
        MAGIC, VERSION, 0x03, seq,
        peak_l, peak_r, rms_l, rms_r,
        rms_mono, rms_side,
        momentary, short_term, integrated,
        gonio_l, gonio_r,
        fft_bins
    )
    payload = bins[:fft_bins].astype(np.float32).tobytes()
    assert len(header) + len(payload) == 1080, f"Packet size mismatch: {len(header) + len(payload)}"
    return header + payload


def to_dbfs(power: float) -> float:
    """Converts linear RMS power to dBFS. Returns -60.0 for silence."""
    if power < 1e-10:
        return -60.0
    return max(10.0 * np.log10(power), -60.0)


def main():
    parser = argparse.ArgumentParser(
        description="Studio Panel audio sender — streams real-time audio to ESP32-S3 via UDP + WebSocket"
    )
    parser.add_argument('--host',     default='192.168.1.100', help='ESP32 IP address')
    parser.add_argument('--port',     type=int, default=UDP_PORT,  help=f'UDP port (default {UDP_PORT})')
    parser.add_argument('--ws-port',  type=int, default=WS_PORT,   help=f'WebSocket server port (default {WS_PORT})')
    parser.add_argument('--device',   default=None,
                        help='Audio device name or index (use --list to see options)')
    parser.add_argument('--list',     action='store_true', help='List audio devices and exit')
    parser.add_argument('--bins',     type=int, default=256,
                        help='FFT bins to send (default: 256; ESP32 only accepts 256)')
    parser.add_argument('--rate',     type=int, default=30,
                        help='Packets per second (default: 30)')
    args = parser.parse_args()

    if args.bins != 256:
        print(f"Warning: ESP32 only accepts 256 bins. Sending {args.bins} may be rejected.", file=sys.stderr)

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

    dev_info = sd.query_devices(dev_idx)
    channels = min(2, dev_info['max_input_channels'])
    if channels < 1:
        print(f"Device '{dev_name}' has no input channels.", file=sys.stderr)
        sys.exit(1)

    blocksize = SR // args.rate
    fft_size  = 1 << (blocksize - 1).bit_length()

    # Start WebSocket server thread before opening audio stream
    ws_thread = threading.Thread(
        target=_ws_thread_main, args=('0.0.0.0', args.ws_port), daemon=True
    )
    ws_thread.start()
    # Brief pause so _ws_loop/_ws_queue are set before audio callback fires
    time.sleep(0.1)

    print(f"Audio source : [{dev_idx}] {dev_name}  (ch={channels})")
    print(f"UDP audio    : {args.host}:{args.port}")
    print(f"WS analysis  : 0.0.0.0:{args.ws_port}  (ESP32 connects as client)")
    print(f"FFT bins     : {args.bins}  |  Rate: {SR} Hz  |  Block: {blocksize} samples (~{blocksize/SR*1000:.0f} ms)")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    window  = np.hanning(fft_size)
    seq     = 0
    i_acc   = 1e-10
    st_acc  = 1e-10
    m_acc   = 1e-10

    ALPHA_M = 1.0 - np.exp(-blocksize / SR / 0.4)
    ALPHA_S = 1.0 - np.exp(-blocksize / SR / 3.0)
    ALPHA_I = 1.0 - np.exp(-blocksize / SR / 30.0)

    def audio_callback(indata, frames, time_info, status):
        nonlocal seq, i_acc, st_acc, m_acc

        if status:
            print(f"[audio] {status}", file=sys.stderr)

        if indata.shape[1] >= 2:
            l = indata[:, 0]
            r = indata[:, 1]
        else:
            l = indata[:, 0]
            r = indata[:, 0]

        mono = (l + r) * 0.5

        peak_l = to_dbfs(float(np.max(np.abs(l))) ** 2)
        peak_r = to_dbfs(float(np.max(np.abs(r))) ** 2)

        # Save linear RMS before converting to dBFS — needed for analysis
        rms_l_lin = float(np.mean(l ** 2))
        rms_r_lin = float(np.mean(r ** 2))
        rms_l = to_dbfs(rms_l_lin)
        rms_r = to_dbfs(rms_r_lin)

        # True M/S RMS — computed from samples, not from per-channel RMS.
        # This correctly captures phase relationships: out-of-phase L+R → silent mono.
        rms_mono = to_dbfs(float(np.mean(mono ** 2)))               # (L+R)/2
        rms_side = to_dbfs(float(np.mean(((l - r) * 0.5) ** 2)))   # (L-R)/2

        power  = float(np.mean(mono ** 2))
        m_acc  += ALPHA_M * (power - m_acc)
        st_acc += ALPHA_S * (power - st_acc)
        i_acc  += ALPHA_I * (0.0158 - i_acc)

        momentary  = to_dbfs(m_acc)
        short_term = to_dbfs(st_acc)
        integrated = to_dbfs(i_acc)

        gonio_l = float(l[-1])
        gonio_r = float(r[-1])

        block = np.zeros(fft_size, dtype=np.float64)
        n = min(len(mono), fft_size)
        block[:n] = mono[:n]
        block *= window

        # Raw (pre-log) magnitudes — used for analysis AND for decimation to UDP bins
        fft_mag_raw = np.abs(np.fft.rfft(block))[:args.bins]

        # Log-scale + normalise for UDP packet
        spectrum = np.log1p(fft_mag_raw * 20.0)
        max_val  = spectrum.max()
        if max_val > 0:
            spectrum /= max_val

        pkt = pack_packet(seq, peak_l, peak_r, rms_l, rms_r,
                          rms_mono, rms_side,
                          momentary, short_term, integrated,
                          gonio_l, gonio_r, spectrum.astype(np.float32),
                          fft_bins=args.bins)
        sock.sendto(pkt, (args.host, args.port))
        seq += 1

        # Push analysis to WebSocket broadcast queue (thread-safe)
        if _ws_loop is not None and _ws_queue is not None:
            analysis = _compute_analysis(fft_mag_raw, rms_l_lin, rms_r_lin)
            _ws_loop.call_soon_threadsafe(_ws_queue.put_nowait, analysis)

    try:
        with sd.InputStream(device=dev_idx, channels=channels, samplerate=SR,
                            blocksize=blocksize, dtype='float32',
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
