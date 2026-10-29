#!/usr/bin/env python3
# Minimal virtual Xbox360 gamepad via /dev/uinput (raw ctypes, no deps).
# Usage: vpad.py <seq>   where seq is space/comma list of buttons to tap, e.g.
#   vpad.py A A A        (tap A three times)
#   vpad.py START B up   (buttons: A B X Y LB RB START SELECT up down left right)
# Keeps the device alive for the whole run so SDL/Proton can enumerate it.
import ctypes, struct, time, sys, fcntl, os

UINPUT = "/dev/uinput"
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT = 0
# button codes
BTN = dict(A=0x130, B=0x131, X=0x133, Y=0x134, LB=0x136, RB=0x137,
           SELECT=0x13a, START=0x13b, MODE=0x13c, THUMBL=0x13d, THUMBR=0x13e)
ABS = dict(X=0, Y=1, Z=2, RX=3, RY=4, RZ=5, HAT0X=0x10, HAT0Y=0x11)

def IOW(nr, size): return (1 << 30) | (size << 16) | (ord('U') << 8) | nr
UI_SET_EVBIT  = IOW(100, 4)
UI_SET_KEYBIT = IOW(101, 4)
UI_SET_ABSBIT = IOW(103, 4)
UI_DEV_CREATE  = (ord('U') << 8) | 1
UI_DEV_DESTROY = (ord('U') << 8) | 2

fd = os.open(UINPUT, os.O_WRONLY | os.O_NONBLOCK)
for b in BTN.values():
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)
for b in BTN.values(): fcntl.ioctl(fd, UI_SET_KEYBIT, b)
for a in ABS.values(): fcntl.ioctl(fd, UI_SET_ABSBIT, a)

# struct uinput_user_dev: name[80], input_id(8), ff_effects_max(u32), absmax/min/fuzz/flat[64] each
name = b"Microsoft X-Box 360 pad"
ABS_CNT = 64
buf = bytearray()
buf += name.ljust(80, b"\0")
buf += struct.pack("<HHHH", 3, 0x045e, 0x028e, 0x0110)  # bus USB, MS vendor, 360 product
buf += struct.pack("<I", 0)  # ff_effects_max
absmax = [0]*ABS_CNT; absmin = [0]*ABS_CNT; absfuzz=[0]*ABS_CNT; absflat=[0]*ABS_CNT
for a in (ABS['X'],ABS['Y'],ABS['RX'],ABS['RY']): absmax[a]=32767; absmin[a]=-32768
for a in (ABS['Z'],ABS['RZ']): absmax[a]=255; absmin[a]=0
for a in (ABS['HAT0X'],ABS['HAT0Y']): absmax[a]=1; absmin[a]=-1
buf += struct.pack("<%di" % ABS_CNT, *absmax)
buf += struct.pack("<%di" % ABS_CNT, *absmin)
buf += struct.pack("<%di" % ABS_CNT, *absfuzz)
buf += struct.pack("<%di" % ABS_CNT, *absflat)
os.write(fd, bytes(buf))
fcntl.ioctl(fd, UI_DEV_CREATE)

def emit(t, c, v):
    os.write(fd, struct.pack("<qqHHi", 0, 0, t, c, v))
def syn(): emit(EV_SYN, SYN_REPORT, 0)
def tap(btn, hold=0.12):
    if btn in BTN:
        emit(EV_KEY, BTN[btn], 1); syn(); time.sleep(hold)
        emit(EV_KEY, BTN[btn], 0); syn()
    elif btn in ("up","down","left","right"):
        ax = ABS['HAT0Y'] if btn in ("up","down") else ABS['HAT0X']
        val = -1 if btn in ("up","left") else 1
        emit(EV_ABS, ax, val); syn(); time.sleep(hold)
        emit(EV_ABS, ax, 0); syn()

time.sleep(float(os.environ.get("VPAD_WAIT", "2.0")))   # let SDL/Proton enumerate the new pad
seq = []
for a in sys.argv[1:]:
    seq += a.replace(",", " ").split()
for b in seq:
    tap(b.upper() if b.upper() in BTN else b.lower())
    time.sleep(0.45)
time.sleep(1.0)
fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
print("done:", seq)
