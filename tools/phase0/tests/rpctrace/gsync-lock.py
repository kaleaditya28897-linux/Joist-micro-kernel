import threading
lock = threading.Lock(); n = [0]
def work():
    for _ in range(2000):
        with lock:
            n[0] += 1
ts = [threading.Thread(target=work) for _ in range(4)]
[t.start() for t in ts]; [t.join() for t in ts]
print("count", n[0])
