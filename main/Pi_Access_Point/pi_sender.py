import socket
import time

PORT    = 5000
PAYLOAD = bytes([0xFF, 0xFF, 0xFF, 0xFF])

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

print("Sending UDP broadcast on port", PORT)
while True:
    try:
        sock.sendto(PAYLOAD, ("255.255.255.255", PORT))
    except Exception as ex:
        print("Error:", ex)
    time.sleep(0.1)   # 10 packets/sec = 100ms interval