#!/usr/bin/env python3
"""Phase 0 measurements against a running Xapiand node.

  convert  CORPUS_JSON OUT_NDJSON N     first N benchmark documents as RESTORE input
  restore  TARGET INDEX NDJSON          bulk-RESTORE and report documents per second
  latency  TARGET INDEX QUERIES N       sequential single-client latency over N queries
  mixed    TARGET INDEX QUERIES SECONDS light-query latency while heavy clients run

QUERIES is the search-benchmark-game queries.txt (JSON lines with a query field). Light
queries fetch one document by id; heavy queries are full-text disjunctions with an
exact count. Results print as JSON.
"""
import http.client
import json
import random
import statistics
import sys
import threading
import time


def connect(target):
    host, port = target.split(":")
    return http.client.HTTPConnection(host, int(port), timeout=600)


def request(conn, method, path, body=None):
    headers = {"Content-Type": "application/json"} if body is not None else {}
    conn.request(method, path, body=body, headers=headers)
    resp = conn.getresponse()
    return resp.status, resp.read()


def percentiles(values):
    values = sorted(values)
    at = lambda q: values[min(len(values) - 1, int(q * (len(values) - 1)))]
    return {"n": len(values), "mean_us": round(statistics.mean(values), 1), "p50_us": round(at(0.5), 1),
            "p95_us": round(at(0.95), 1), "p99_us": round(at(0.99), 1), "max_us": round(values[-1], 1)}


def convert(corpus, out, n):
    with open(corpus) as src, open(out, "w") as dst:
        for i, line in enumerate(src):
            if i >= n:
                break
            doc = json.loads(line)
            dst.write(json.dumps({"_id": str(i), "url": doc["id"], "text": doc["text"], "sort_field": doc["sort_field"]}) + "\n")


def restore(target, index, ndjson):
    with open(ndjson, "rb") as f:
        body = f.read()
    ndocs = body.count(b"\n")
    conn = connect(target)
    t0 = time.perf_counter()
    conn.request("RESTORE", f"/{index}/?commit", body=body, headers={"Content-Type": "application/x-ndjson"})
    resp = conn.getresponse()
    resp.read()
    elapsed = time.perf_counter() - t0
    print(json.dumps({"docs": ndocs, "seconds": round(elapsed, 2), "docs_per_sec": round(ndocs / elapsed), "status": resp.status}))


def read_queries(path):
    """The harness's queries.txt: JSON lines with a "query" field; operators dropped."""
    queries = []
    with open(path) as f:
        for line in f:
            text = json.loads(line)["query"].replace('"', "").replace("+", "").replace("-", " ")
            if text.strip():
                queries.append(" ".join(text.split()))
    return queries


def search(conn, index, text, limit=10, check_at_least=0):
    body = {"_query": {"text": text}, "_limit": limit}
    if check_at_least:
        body["_check_at_least"] = check_at_least
    return request(conn, "SEARCH", f"/{index}/", json.dumps(body).encode())


def latency(target, index, queries_path, n):
    queries = read_queries(queries_path)[:n]
    conn = connect(target)
    for q in queries[:50]:  # warm-up
        search(conn, index, q)
    samples, errors = [], 0
    for q in queries:
        t0 = time.perf_counter()
        status, _ = search(conn, index, q)
        samples.append((time.perf_counter() - t0) * 1e6)
        errors += status != 200
    out = percentiles(samples)
    out["errors"] = errors
    print(json.dumps(out))


def mixed(target, index, queries_path, seconds, heavy_clients=8, docs=100000):
    queries = read_queries(queries_path)
    stop = time.perf_counter() + seconds
    heavy_done = [0]
    lock = threading.Lock()

    def heavy(seed):
        rng = random.Random(seed)
        conn = connect(target)
        n = 0
        while time.perf_counter() < stop:
            q = " ".join(rng.sample(queries, 4))  # a broad disjunction
            search(conn, index, q, limit=100, check_at_least=1000000)
            n += 1
        with lock:
            heavy_done[0] += n

    def light_once(conn, rng):
        t0 = time.perf_counter()
        status, _ = request(conn, "GET", f"/{index}/{rng.randrange(docs)}")
        return (time.perf_counter() - t0) * 1e6, status

    # Unloaded light latency first.
    rng = random.Random(1)
    conn = connect(target)
    unloaded = [light_once(conn, rng)[0] for _ in range(2000)]

    threads = [threading.Thread(target=heavy, args=(s,)) for s in range(heavy_clients)]
    for t in threads:
        t.start()
    loaded, errors = [], 0
    while time.perf_counter() < stop:
        us, status = light_once(conn, rng)
        loaded.append(us)
        errors += status != 200
    for t in threads:
        t.join()
    print(json.dumps({"heavy_clients": heavy_clients, "heavy_queries": heavy_done[0], "light_unloaded": percentiles(unloaded),
                      "light_under_heavy": percentiles(loaded), "light_errors": errors}))


if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "convert":
        convert(args[0], args[1], int(args[2]))
    elif cmd == "restore":
        restore(*args)
    elif cmd == "latency":
        latency(args[0], args[1], args[2], int(args[3]))
    elif cmd == "mixed":
        mixed(args[0], args[1], args[2], float(args[3]))
    else:
        sys.exit(__doc__)
