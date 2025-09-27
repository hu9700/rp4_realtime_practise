import pandas as pd
import matplotlib.pyplot as plt
import sys
fn = sys.argv[1]
df = pd.read_csv(fn)
lat_ms = df['latency_ns'] / 1e6
plt.figure(figsize=(7,4))
plt.hist(lat_ms, bins=200, range=(-1,10))
plt.xlabel('latency (ms)')
plt.ylabel('count')
plt.title(fn)
plt.grid(True)
plt.show()