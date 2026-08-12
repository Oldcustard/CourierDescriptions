"""
Build a silent .fuz voice file of a chosen duration.

WHY THIS EXISTS
    Our courier line is an unvoiced dialogue INFO. SkyrimNet generates TTS for it,
    but generation is not instant (~4.9 s measured against a cloud TTS endpoint)
    and NOTHING pauses the vanilla dialogue chain while that happens - the engine
    advances on its own clock. Whatever audio file sits at the INFO's voice path
    defines how long the engine holds that dialogue slot, so a silent file of a
    known length is what buys the generated speech time to arrive and finish.

    Fuz Ro D-oh does this generically for every unvoiced line, but its hold is
    capped at 10 s and is tuned by a GLOBAL ini setting affecting every mod. A
    file of our own at our own INFO's path is scoped to just this line, is not
    capped, and needs no third-party mod installed.

FORMAT (verified against a real .fuz on disk)
    'FUZE'              4 bytes magic
    version             uint32, = 1
    lipSize             uint32, bytes of LIP data that follow
    <lip data>          lipSize bytes  - omitted here (lipSize = 0)
    <xWMA audio>        'RIFF' .... 'XWMA' - the rest of the file

    LIP data drives vanilla mouth movement. We write none: SkyrimNet does its own
    lip sync over generated speech (its ExpressionManager reported 63 viseme
    changes on the probe run), so a LIP track here would be redundant and would
    describe the wrong words anyway.

USAGE
    python make_silent_fuz.py <seconds> <output.fuz> [--xwmaencode PATH]
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

SAMPLE_RATE = 44100
BITS_PER_SAMPLE = 16
CHANNELS = 1


def write_silent_wav(path, seconds):
    """A PCM WAV of pure silence - xwmaencode's input."""
    frames = int(SAMPLE_RATE * seconds)
    data_bytes = frames * CHANNELS * (BITS_PER_SAMPLE // 8)
    byte_rate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE // 8)
    block_align = CHANNELS * (BITS_PER_SAMPLE // 8)

    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_bytes))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, CHANNELS, SAMPLE_RATE,
                            byte_rate, block_align, BITS_PER_SAMPLE))
        f.write(b"data")
        f.write(struct.pack("<I", data_bytes))
        # Silence is zeroes; write in chunks so a long file doesn't balloon RAM.
        remaining = data_bytes
        chunk = b"\x00" * 65536
        while remaining > 0:
            f.write(chunk[:min(remaining, len(chunk))])
            remaining -= min(remaining, len(chunk))


def encode_xwm(wav_path, xwm_path, xwmaencode):
    result = subprocess.run([xwmaencode, wav_path, xwm_path],
                            capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(xwm_path):
        sys.exit("xwmaencode failed (%d):\n%s\n%s"
                 % (result.returncode, result.stdout, result.stderr))


def wrap_fuz(xwm_path, fuz_path):
    with open(xwm_path, "rb") as f:
        xwm = f.read()
    if xwm[:4] != b"RIFF":
        sys.exit("encoded file is not RIFF/XWMA - refusing to wrap it")
    with open(fuz_path, "wb") as f:
        f.write(b"FUZE")
        f.write(struct.pack("<I", 1))   # version
        f.write(struct.pack("<I", 0))   # lipSize = no LIP track
        f.write(xwm)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seconds", type=float)
    ap.add_argument("output")
    ap.add_argument("--xwmaencode", default=None)
    args = ap.parse_args()

    xwmaencode = args.xwmaencode
    if not xwmaencode or not os.path.exists(xwmaencode):
        sys.exit("need --xwmaencode PATH to xwmaencode.exe")

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "silent.wav")
        xwm = os.path.join(tmp, "silent.xwm")
        write_silent_wav(wav, args.seconds)
        encode_xwm(wav, xwm, xwmaencode)
        wrap_fuz(xwm, args.output)

    print("wrote %s (%.1f s of silence, %d bytes)"
          % (args.output, args.seconds, os.path.getsize(args.output)))


if __name__ == "__main__":
    main()
