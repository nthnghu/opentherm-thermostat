import serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
s.write(("\nGO " + sys.argv[2] + "\n").encode())
time.sleep(float(sys.argv[3]) if len(sys.argv) > 3 else 0.8)
