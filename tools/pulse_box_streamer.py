"""
PulseBox Audio Streamer
Captures system audio (WASAPI loopback) and streams it over UDP to the ESP32.

Usage:
    python pulse_box_streamer.py                  # uses mDNS (pulsebox.local)
    python pulse_box_streamer.py 192.168.1.42     # hardcoded IP

Dependencies:
    pip install pyaudiowpatch numpy
"""

import sys
import struct
import socket
import numpy as np
import pyaudiowpatch as pyaudio

# Must match wifi_audio.h on the ESP32
UDP_PORT = 5000
SAMPLE_RATE = 48000
SAMPLES_PER_PKT = 512
MAGIC = 0x5042

TARGET_HOST = sys.argv[1] if len(sys.argv) > 1 else "pulsebox.local"


def find_wasapi_loopback(p):
    """Find the default output device's WASAPI loopback."""
    try:
        wasapi_info = p.get_host_api_info_by_type(pyaudio.paWASAPI)
    except OSError:
        print("ERROR: WASAPI not available on this system")
        sys.exit(1)

    # Get the default output device
    default_output = p.get_device_info_by_index(wasapi_info["defaultOutputDevice"])
    print(f"Default output device: {default_output['name']}")

    # Find its loopback counterpart
    for i in range(p.get_device_count()):
        dev = p.get_device_info_by_index(i)
        if dev["hostApi"] == wasapi_info["index"] and dev.get("isLoopbackDevice", False):
            if dev["name"].startswith(default_output["name"]):
                print(f"Found loopback device: [{i}] {dev['name']}")
                return dev

    # Fallback: any loopback device
    for i in range(p.get_device_count()):
        dev = p.get_device_info_by_index(i)
        if dev["hostApi"] == wasapi_info["index"] and dev.get("isLoopbackDevice", False):
            print(f"Found loopback device (fallback): [{i}] {dev['name']}")
            return dev

    print("ERROR: No WASAPI loopback device found")
    sys.exit(1)


def main():
    p = pyaudio.PyAudio()
    loopback_dev = find_wasapi_loopback(p)

    # Resolve target address
    try:
        target_ip = socket.gethostbyname(TARGET_HOST)
    except socket.gaierror:
        print(f"ERROR: Could not resolve '{TARGET_HOST}'")
        print("Pass the ESP32's IP address as a command-line argument instead.")
        p.terminate()
        sys.exit(1)

    print(f"Streaming to {TARGET_HOST} ({target_ip}:{UDP_PORT})")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = 0
    leftover = np.array([], dtype=np.int16)

    channels = int(loopback_dev["maxInputChannels"])
    device_rate = int(loopback_dev["defaultSampleRate"])

    print(f"Loopback: {loopback_dev['name']} ({channels}ch, {device_rate}Hz)")
    print(f"Streaming as: mono, {SAMPLE_RATE}Hz, {SAMPLES_PER_PKT} samples/packet")
    print("Press Ctrl+C to stop\n")

    def audio_callback(in_data, frame_count, time_info, status):
        nonlocal seq, leftover

        # in_data is raw bytes: float32 interleaved samples
        audio = np.frombuffer(in_data, dtype=np.float32).reshape(-1, channels)

        # Mix down to mono
        if channels > 1:
            mono = audio.mean(axis=1)
        else:
            mono = audio[:, 0]

        # float32 [-1.0, 1.0] -> int16
        samples = np.clip(mono * 32767, -32768, 32767).astype(np.int16)

        # Prepend leftover from previous callback
        if len(leftover) > 0:
            samples = np.concatenate([leftover, samples])
            leftover = np.array([], dtype=np.int16)

        # Send full packets
        offset = 0
        while offset + SAMPLES_PER_PKT <= len(samples):
            chunk = samples[offset:offset + SAMPLES_PER_PKT]
            header = struct.pack("<HH", MAGIC, seq & 0xFFFF)
            sock.sendto(header + chunk.tobytes(), (target_ip, UDP_PORT))
            if seq % 100 == 0:
                peak = np.max(np.abs(chunk))
                print(f"Sent packet seq={seq}, peak={peak}")
            seq += 1
            offset += SAMPLES_PER_PKT

        # Save remainder
        if offset < len(samples):
            leftover = samples[offset:]

        return (None, pyaudio.paContinue)

    stream = p.open(
        format=pyaudio.paFloat32,
        channels=channels,
        rate=device_rate,
        input=True,
        input_device_index=loopback_dev["index"],
        frames_per_buffer=SAMPLES_PER_PKT,
        stream_callback=audio_callback,
    )

    try:
        stream.start_stream()
        while stream.is_active():
            import time
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        stream.stop_stream()
        stream.close()
        p.terminate()


if __name__ == "__main__":
    main()
