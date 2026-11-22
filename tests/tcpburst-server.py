"""Host side of tests/aros/tcpburst.c: for each connection, send BYTES at
once and close. Logs per connection how long sendall took.

    python3 tests/tcpburst-server.py PORT BYTES CONNECTIONS
"""
import socket
import sys
import time

port, size, count = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(4)
payload = bytes(i % 251 for i in range(size))
for n in range(count):
    c, addr = s.accept()
    t = time.monotonic()
    c.sendall(payload)
    print(f"connection {n + 1}: {size} bytes sent in {time.monotonic() - t:.3f} s", flush=True)
    c.shutdown(socket.SHUT_WR)
    c.recv(1)
    c.close()
