"""Linux black-box public tests. Usage: python3 tests/test_public.py build/log_filter."""
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import unittest

EXE = str(pathlib.Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 else ''
HEADER = struct.Struct('<8sIIQ')
RECORD = struct.Struct('<QIIII')

def encode(rows):
    return HEADER.pack(b'NCULOG01', 1, 24, len(rows)) + b''.join(RECORD.pack(*r) for r in rows)

def decode(data):
    magic, version, width, count = HEADER.unpack_from(data)
    assert (magic, version, width) == (b'NCULOG01', 1, 24)
    assert len(data) == 24 + count * 24
    return [RECORD.unpack_from(data, 24 + i*24) for i in range(count)]

def reference(rows, query):
    begin, end, latency, flags = query
    return [r for r in rows if begin <= r[0] < end and r[2] >= latency and r[4] & flags == flags]

class PublicTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='ncuscc-stage3-')
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)

    def run_program(self, *args, ok=True):
        result = subprocess.run([EXE, *map(str, args)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0 if ok else 2, result.stdout + result.stderr)
        return result

    def compare(self, rows, query=(1000, 2000, 100, 3)):
        src, dst = self.root/'in.bin', self.root/'out.bin'
        src.write_bytes(encode(rows))
        if dst.exists():
            dst.unlink()
        result = self.run_program('--input', src, '--output', dst, '--repeats', 2,
                                  '--begin', query[0], '--end', query[1],
                                  '--min-latency', query[2], '--flags', query[3])
        self.assertEqual(decode(dst.read_bytes()), reference(rows, query))
        self.assertEqual(src.read_bytes(), encode(rows), 'input file changed')
        self.assertEqual(result.stdout.count('check=PASS'), 2)

    def test_empty_single_and_extremes(self):
        for rows in [[], [(1000, 7, 100, 123, 3)], [(2000, 1, 100, 2, 3)],
                     [(1500, i, 101, i*19, 3) for i in range(67)],
                     [(1500, i, 99, i*19, 3) for i in range(67)]]:
            with self.subTest(size=len(rows)):
                self.compare(rows)

    def test_boundaries_order_duplicates_and_flags(self):
        rows = [(2000,1,100,11,3),(1000,2,100,22,3),(999,3,100,33,3),
                (1999,4,99,44,3),(1500,5,100,55,1),(1100,6,100,66,7),
                (1000,2,100,22,3),(1200,8,100,88,2)]
        for query in [(1000,2000,100,3),(1000,2000,100,0),(1000,1000,0,0)]:
            self.compare(rows,query)
        self.compare([(2**64-2,2**32-1,2**32-1,2**32-1,2**32-1),
                      (2**64-1,0,0,0,0)], (2**64-2,2**64-1,2**32-1,2**32-1))

    def test_random_differential_and_tails(self):
        for seed in range(8):
            rng = random.Random(seed)
            rows = [(rng.randrange(900,2100),i,rng.randrange(80,130),rng.getrandbits(32),rng.randrange(16))
                    for i in range(257+seed)]
            self.compare(rows)

    def test_generator_reproducibility_and_distribution(self):
        for pattern in ['random','blocks']:
            for rate in [0,1,50,90,100]:
                paths=[]
                for run in range(2):
                    src=self.root/f'{pattern}-{rate}-{run}.bin'
                    dst=self.root/f'{pattern}-{rate}-{run}-out.bin'
                    self.run_program('--count',10001,'--seed',17,'--pattern',pattern,
                                     '--hit-percent',rate,'--repeats',1,'--export-input',src,'--output',dst)
                    rows=decode(src.read_bytes())
                    self.assertEqual(decode(dst.read_bytes()),reference(rows,(1000,2000,100,3)))
                    hits=len(reference(rows,(1000,2000,100,3)))
                    if rate in [0,100]: self.assertEqual(hits,10001*rate//100)
                    paths.append(src)
                self.assertEqual(paths[0].read_bytes(),paths[1].read_bytes())

    def test_invalid_arguments(self):
        for args in [('--count','-1'),('--repeats','0'),('--flags',str(2**32)),
                     ('--begin','5','--end','4'),('--hit-percent','101'),
                     ('--pattern','bad'),('--count','10','--max-records','9'),
                     ('--count','1','--count','2'),('--unknown','1'),('--count',),
                     ('--input',''),('--count','18446744073709551616')]:
            with self.subTest(args=args): self.run_program(*args,ok=False)

    def test_malformed_files(self):
        good=encode([(1000,1,100,2,3)])
        bads=[b'',good[:23],good[:-1],good+b'x',b'BADMAGIC'+good[8:],
              HEADER.pack(b'NCULOG01',2,24,1)+good[24:],
              HEADER.pack(b'NCULOG01',1,32,1)+good[24:],
              HEADER.pack(b'NCULOG01',1,24,2**64-1)]
        for i,data in enumerate(bads):
            src=self.root/f'bad{i}.bin'; src.write_bytes(data)
            self.run_program('--input',src,ok=False)

    def test_existing_outputs_and_file_limit(self):
        src=self.root/'input.bin'; src.write_bytes(encode([(1000,1,100,2,3)]))
        before=src.read_bytes()
        self.run_program('--input',src,'--output',src,'--repeats',1,ok=False)
        self.assertEqual(src.read_bytes(),before)
        self.run_program('--input',src,'--max-records',0,ok=False)
        self.run_program('--input',src,'--count',1,ok=False)
        dst=self.root/'existing.bin'; dst.write_bytes(b'keep')
        self.run_program('--count',0,'--output',dst,'--repeats',1,ok=False)
        self.assertEqual(dst.read_bytes(),b'keep')

if __name__ == '__main__':
    if not EXE:
        raise SystemExit('Provide the compiled executable path.')
    unittest.main()
