#!/usr/bin/env python3
"""Real GPU bridge-only diagnosis; never calls seekdb, VSAG or SQL fallback."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import threading
import time
import traceback
import numpy as np


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--dataset', type=Path, required=True)
    parser.add_argument('--queries', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists(): parser.error('Output must be fresh')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    base = np.ascontiguousarray(np.load(args.dataset/'base.npy'), dtype=np.float32)
    queries = np.ascontiguousarray(np.load(args.dataset/'queries.npy')[:args.queries], dtype=np.float32)
    ids = np.load(args.dataset/'ids.npy')
    truth = np.load(args.dataset/'truth.npy')[:len(queries)]
    valid = set(map(int, ids))
    report = {'scope':'bridge-only GPU diagnostic; no SQL; no CPU fallback', 'status':'FAIL',
              'dataset':str(args.dataset), 'shape':list(base.shape), 'queries':len(queries),
              'sample':'all' if len(queries)==len(np.load(args.dataset/'queries.npy')) else 'prefix pilot',
              'source_sha256':digest(args.source), 'bridge_sha256':digest(args.bridge)}
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    bridge = ctypes.CDLL(str(args.bridge.resolve()))
    ptr = ctypes.c_void_p
    bridge.seekdb_cuvs_build.argtypes=[ptr,ctypes.c_long,ctypes.c_long]
    bridge.seekdb_cuvs_build.restype=ptr
    bridge.seekdb_cuvs_search.argtypes=[ptr,ptr,ctypes.c_long,ctypes.c_long,ptr,ptr]
    bridge.seekdb_cuvs_search.restype=ctypes.c_int
    bridge.seekdb_cuvs_free.argtypes=[ptr]

    def run():
        handle = None
        try:
            start = time.perf_counter()
            handle = bridge.seekdb_cuvs_build(base.ctypes.data,len(base),base.shape[1])
            report['build_seconds'] = time.perf_counter()-start
            if not handle: raise RuntimeError('Real GPU build returned NULL')
            for k in (10,100):
                offsets = np.zeros((1,k),dtype=np.uint32)
                distances = np.zeros((1,k),dtype=np.float32)
                total = 0.0
                hits = 0
                latencies=[]
                for q, query in enumerate(queries):
                    start=time.perf_counter()
                    rc=bridge.seekdb_cuvs_search(handle,query.ctypes.data,1,k,offsets.ctypes.data,distances.ctypes.data)
                    elapsed=time.perf_counter()-start
                    if rc: raise RuntimeError(f'Real GPU search failed: k={k}, q={q}, rc={rc}; no CPU fallback')
                    if k==10 and q==0:
                        report['first_query_seconds']=elapsed
                        report['build_plus_first_query_seconds']=report['build_seconds']+elapsed
                    total+=elapsed
                    latencies.append(elapsed)
                    if np.any(offsets>=len(base)): raise ValueError('GPU row offset out of range')
                    found=ids[offsets[0]]
                    if len(set(map(int,found)))!=k or any(int(i) not in valid for i in found):
                        raise ValueError('Duplicate or illegal ID')
                    if not np.isfinite(distances).all(): raise ValueError('Nonfinite GPU distance')
                    if q==0:
                        exact=np.sum((base[offsets[0]].astype(np.float64)-query.astype(np.float64))**2,axis=1)
                        np.testing.assert_allclose(distances[0],exact,rtol=3e-5,atol=1e-4)
                    hits+=len(set(map(int,found)).intersection(map(int,truth[q,:k])))
                report[f'k{k}']={'recall':hits/(len(queries)*k),'qps':len(queries)/total,
                                'seconds':total,'p50_ms':float(np.median(latencies)*1000),
                                'p95_ms':float(np.percentile(latencies,95)*1000)}
                args.output.write_text(json.dumps(report,indent=2)+'\n')
                print(json.dumps({f'k{k}':report[f'k{k}']}),flush=True)
            report['status']='PASS'
        except Exception:
            report['error']=traceback.format_exc()
        finally:
            if handle: bridge.seekdb_cuvs_free(handle)
    threading.stack_size(32*1024*1024)
    worker=threading.Thread(target=run)
    worker.start()
    worker.join()
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)
    return 0 if report['status']=='PASS' else 1


if __name__=='__main__': raise SystemExit(main())
