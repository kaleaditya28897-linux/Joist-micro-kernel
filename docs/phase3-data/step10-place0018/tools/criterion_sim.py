#!/usr/bin/env python3
# criterion_sim.py : for normally distributed boot p50s with a given CV, how
# often is (max-min)/median < 5 % in a 10-boot window, and in every one of
# the 11 windows of 20 consecutive boots (the gate's campaign length)?
import random, statistics as st
random.seed(1)
for cv in (0.8, 1.0, 1.2, 1.4, 1.5, 1.7, 2.0, 2.5, 3.0):
    one = every = 0
    T = 20000
    for _ in range(T):
        v = [random.gauss(100, cv) for _ in range(20)]
        w = [(max(v[i:i+10]) - min(v[i:i+10])) / st.median(v[i:i+10]) * 100 for i in range(11)]
        one += w[0] < 5
        every += max(w) < 5
    print(f"CV {cv:.1f} %: P(one 10-window < 5 %) = {one/T:.2f}; P(all 11 windows of 20 boots < 5 %) = {every/T:.2f}")
