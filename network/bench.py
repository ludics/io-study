import socket, threading, time, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18000
N_THREADS = int(sys.argv[2]) if len(sys.argv) > 2 else 8
SIZE = int(sys.argv[3]) if len(sys.argv) > 3 else 512
DURATION = int(sys.argv[4]) if len(sys.argv) > 4 else 5

qps = [0] * N_THREADS
stop = False

def worker(idx):
    s = socket.socket()
    s.connect(('127.0.0.1', PORT))
    data = b'A' * SIZE
    local = 0
    while not stop:
        try:
            s.sendall(data)
            buf = b''
            while len(buf) < SIZE:
                chunk = s.recv(SIZE - len(buf))
                if not chunk:
                    return
                buf += chunk
            if buf == data:
                local += 1
        except Exception:
            return
    qps[idx] = local
    s.close()

threads = [threading.Thread(target=worker, args=(i,)) for i in range(N_THREADS)]
for t in threads:
    t.start()

time.sleep(DURATION)
stop = True
for t in threads:
    t.join()

total = sum(qps)
print(f"=== 压测结果 ===")
print(f"线程数: {N_THREADS}, 数据大小: {SIZE}字节, 时长: {DURATION}s")
print(f"总请求数: {total}")
print(f"平均 QPS: {total // DURATION}")
print(f"每线程: {qps}")
