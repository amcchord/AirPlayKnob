"""Decode firmware ALAC packets with FFmpeg, using both RAOP and default cookies.

Run: python3 tests/test_alac_packets.py [path/to/raop_AnySpeaker.cpp]
Historical comparisons: add --frames 352 --decoder-frame-length 352 (or 4096).
Requires a host C++ compiler and ffmpeg. All generated files stay in scratch/.
"""
import argparse
import pathlib
import random
import shutil
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', nargs='?', type=pathlib.Path, default=ROOT / 'src/audio/raop_AnySpeaker.cpp')
parser.add_argument('--frames', nargs='+', type=int, choices=(1, 352), default=[1, 352])
parser.add_argument('--decoder-frame-length', nargs='+', type=int, choices=(352, 4096), default=[352, 4096])
args = parser.parse_args()
source = args.source
text = source.read_text()
start = text.index('static int encode_pcm_to_alac_raw(')
end = text.index('// --- SRP-6a', start)
function = text[start:end]
for tool in ('c++', 'ffmpeg'):
    if not shutil.which(tool):
        raise SystemExit(f'Required tool missing: {tool}')

def chunk(tag, data):
    return tag + struct.pack('>q', len(data)) + data

def packet_size(value):
    result = [value & 127]
    value >>= 7
    while value:
        result.insert(0, 128 | (value & 127))
        value >>= 7
    return bytes(result)

def caf(packet, frames, decoder_frame_length):
    desc = struct.pack('>d4sIIIII', 44100, b'alac', 0, 0, frames, 2, 0)
    config = struct.pack('>I6BHIII', decoder_frame_length, 0, 16, 40, 10, 14, 2, 0, 0, 0, 44100)
    cookie = struct.pack('>I4s4sI4sI', 12, b'frma', b'alac', 36, b'alac', 0) + config
    table = struct.pack('>qqii', 1, frames, 0, 0) + packet_size(len(packet))
    return (b'caff\0\1\0\0' + chunk(b'desc', desc) + chunk(b'kuki', cookie)
            + chunk(b'data', bytes(4) + packet) + chunk(b'pakt', table))

(ROOT / 'scratch').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='alac-test-', dir=ROOT / 'scratch') as directory:
    temp = pathlib.Path(directory)
    host = temp / 'encode.cpp'
    host.write_text('#include <cstdint>\n#include <cstdio>\n#include <cstdlib>\n'
                    '#define RAOP_CHUNK_FRAMES 352\n' + function + '''
int main(int argc, char **argv) {
    int16_t pcm[704]; uint8_t out[1424];
    for (auto &b : out) b = 0xA5;
    if (fread(pcm, sizeof(pcm), 1, stdin) != 1) return 1;
    int n = encode_pcm_to_alac_raw(pcm, atoi(argv[1]), out);
    fwrite(out, 1, n, stdout);
}
''')
    subprocess.run(['c++', '-Wall', '-Wextra', '-Wno-unused-parameter', str(host), '-o', str(temp / 'encode')], check=True)
    rng = random.Random(42)
    patterns = {
        'silence': [0] * 704,
        'extremes': [-32768, 32767, -1, 0, 1, 16384, -16384, 42] * 88,
        'stereo_noise': [rng.randint(-32768, 32767) for _ in range(704)],
    }
    checks = 0
    for name, samples in patterns.items():
        pcm = struct.pack('<704h', *samples)
        for frames in args.frames:
            packet = subprocess.check_output([str(temp / 'encode'), str(frames)], input=pcm)
            for cookie_frames in args.decoder_frame_length:
                (temp / 'packet.caf').write_bytes(caf(packet, frames, cookie_frames))
                result = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i',
                    str(temp / 'packet.caf'), '-f', 's16le', '-y', str(temp / 'decoded.raw')],
                    capture_output=True, text=True)
                assert result.returncode == 0, (name, frames, cookie_frames, result.stderr)
                assert (temp / 'decoded.raw').read_bytes() == pcm[:frames * 4], (name, frames, cookie_frames)
                checks += 1
    print(f'PASS: {checks} bit-exact ALAC decodes (packet frames={args.frames}; decoder defaults={args.decoder_frame_length}).')
