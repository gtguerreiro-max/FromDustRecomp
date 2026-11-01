"""Launch the runtime under the Windows debugger API and report native crashes.

Usage: python tools/debug_launch.py [--seconds 20] [game directory]
Useful for fail-fast exceptions, which bypass the in-process crash handler.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import os
import struct
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--seconds', type=int, default=20)
parser.add_argument('game', nargs='?')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
k32 = c.WinDLL('kernel32', use_last_error=True)
dbg = c.WinDLL('dbghelp', use_last_error=True)

class ExceptionRecord(c.Structure):
    _fields_ = [('code', w.DWORD), ('flags', w.DWORD), ('record', c.c_void_p),
                ('address', c.c_void_p), ('count', w.DWORD), ('info', c.c_size_t * 15)]

class ExceptionInfo(c.Structure):
    _fields_ = [('record', ExceptionRecord), ('first_chance', w.DWORD)]

class EventData(c.Union):
    _fields_ = [('exception', ExceptionInfo), ('raw', c.c_ubyte * 160)]

class DebugEvent(c.Structure):
    _fields_ = [('code', w.DWORD), ('process', w.DWORD), ('thread', w.DWORD), ('data', EventData)]

k32.WaitForDebugEvent.argtypes = [c.POINTER(DebugEvent), w.DWORD]
k32.ContinueDebugEvent.argtypes = [w.DWORD, w.DWORD, w.DWORD]
k32.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.OpenThread.restype = w.HANDLE
k32.GetThreadContext.argtypes = [w.HANDLE, c.c_void_p]
k32.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
k32.CloseHandle.argtypes = [w.HANDLE]
dbg.SymInitialize.argtypes = [w.HANDLE, c.c_char_p, w.BOOL]
dbg.SymFromAddr.argtypes = [w.HANDLE, c.c_uint64, c.POINTER(c.c_uint64), c.c_void_p]
dbg.SymSetOptions(0x2 | 0x4 | 0x10 | 0x200)
env = os.environ.copy()
env.update(FD_RENDERER='d3d12', FD_HEADLESS='1', FD_TRACE='0', FD_WATCHDOG='0')
output = open(root / 'build' / 'debug_output.txt', 'w')
p = subprocess.Popen([str(root / 'build' / 'FromDust.exe'), args.game or str(root / 'game')],
                     cwd=root / 'build', env=env, stdout=output, stderr=output,
                     creationflags=0x2 | 0x08000000)
handle = w.HANDLE(int(p._handle))
deadline = time.monotonic() + args.seconds

def symbol(address):
    buf = c.create_string_buffer(88 + 1024)
    struct.pack_into('<I', buf, 0, 88)
    struct.pack_into('<I', buf, 80, 1024)
    displacement = c.c_uint64()
    if dbg.SymFromAddr(handle, address, c.byref(displacement), buf):
        name = buf.raw[84:].split(b'\0', 1)[0].decode(errors='replace')
        return f'{name}+0x{displacement.value:x}'
    return None

try:
    while time.monotonic() < deadline:
        event = DebugEvent()
        if not k32.WaitForDebugEvent(c.byref(event), 100):
            continue
        status = 0x00010002  # DBG_CONTINUE
        if event.code == 1:
            exception = event.data.exception
            code = exception.record.code
            if code not in (0x80000003, 0x406D1388):
                status = 0x80010001  # DBG_EXCEPTION_NOT_HANDLED
                if code == 0xC0000409 or not exception.first_chance:
                    print(f'Exception 0x{code:08X}, thread {event.thread}, '
                          f'address 0x{exception.record.address:X}, '
                          f'parameters {list(exception.record.info)[:exception.record.count]}', flush=True)
                    dbg.SymInitialize(handle, str(root / 'build').encode(), True)
                    thread = k32.OpenThread(0x8, False, event.thread)
                    # AMD64 CONTEXT is 1232 bytes, with ContextFlags at +48.
                    context = c.create_string_buffer(1232 + 16)
                    aligned = (c.addressof(context) + 15) & ~15
                    c.c_uint32.from_address(aligned + 48).value = 0x10000B
                    if k32.GetThreadContext(thread, aligned):
                        rip = c.c_uint64.from_address(aligned + 248).value
                        rsp = c.c_uint64.from_address(aligned + 152).value
                        print(f'RIP 0x{rip:X}: {symbol(rip)}; RSP 0x{rsp:X}', flush=True)
                        stack = c.create_string_buffer(8192)
                        count = c.c_size_t()
                        k32.ReadProcessMemory(handle, rsp, stack, len(stack), c.byref(count))
                        for offset in range(0, count.value - 7, 8):
                            value = struct.unpack_from('<Q', stack, offset)[0]
                            if value > 0x100000000:
                                name = symbol(value)
                                if name:
                                    print(f'  stack+0x{offset:03X}: 0x{value:X} {name}', flush=True)
                    k32.CloseHandle(thread)
                    p.kill()
        elif event.code == 5:
            print('Process exited', flush=True)
            break
        k32.ContinueDebugEvent(event.process, event.thread, status)
finally:
    if p.poll() is None:
        p.kill()
    output.close()
