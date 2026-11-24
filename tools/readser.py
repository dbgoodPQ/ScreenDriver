"""Records the C6's USB debug log (needs pyserial; PlatformIO's Python has it).

    python tools/readser.py COM13 600
    python tools/readser.py SER=90:64:9B:AB:49:08 600      # find the XIAO by its USB serial number

Safe to leave attached while benchmarking: the C6 firmware disables the USB interrupt that used to cost
it half its CPU while a program held the port open.
"""
import serial, serial.tools.list_ports, time, sys
# usage: readser.py <COMx | SER=<usb serial>> <seconds>
# With SER=..., the port is looked up again on every reconnect (it can change when the device re-enumerates).
target, secs = sys.argv[1], float(sys.argv[2])
end = time.time() + secs

def find_port():
    if not target.startswith("SER="):
        return target
    want = target[4:].upper()
    for p in serial.tools.list_ports.comports():
        if (p.serial_number or "").upper() == want or want in (p.hwid or "").upper():
            return p.device
    return None

s = None
while time.time() < end:
    if s is None:
        port = find_port()
        if port is None:
            time.sleep(0.3); continue
        try:
            s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.5
            s.dtr = False; s.rts = False; s.open()
            sys.stdout.write("\n[readser] opened %s\n" % port); sys.stdout.flush()
        except Exception:
            s = None; time.sleep(0.3); continue
    try:
        d = s.read(512)
        if d: sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
    except Exception:
        try: s.close()
        except Exception: pass
        s = None; time.sleep(0.3)
