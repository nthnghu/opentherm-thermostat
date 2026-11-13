import serial,time,sys
port,secs,out=sys.argv[1],float(sys.argv[2]),sys.argv[3]
s=serial.Serial(port,115200,timeout=0.2)
s.dtr=False; s.rts=True; time.sleep(0.1); s.rts=False
t=time.time(); buf=b''
while time.time()-t<secs: buf+=s.read(4096)
open(out,'wb').write(buf)
