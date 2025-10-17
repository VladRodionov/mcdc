
# 🥕 MCDC — Memcached with Dictionary Compression

**MCDC** (Memcached Dictionary Compression) is a **drop-in replacement for Memcached 1.6.38** with built-in 
**Zstandard dictionary compression**, delivering 1.5×–2.5× better RAM efficiency without any client-side 
changes. MCDC is developed and maintained by [Carrot Data](https://github.com/carrotdata) — a project focused on practical, 
SSD-friendly, memory-efficient caching technologies.

---

## 🧩 What Is MCDC?

MCDC extends Memcached with adaptive, server-side data compression.  Instead of compressing 
each object individually (like client SDKs usually do), MCDC automatically learns shared byte 
patterns across your data and uses them  to build a **Zstandard dictionary**. Once trained, 
the dictionary allows ultra-compact compression of small-to-medium, structurally similar objects — 
tweets, JSON fragments, log entries, etc. — at minimal CPU cost.

All standard Memcached commands, clients, and SDKs continue to work unchanged.

---

## 🧠 Dictionary Compression — Why It Matters


Typical “client-side compression” operates in isolation: every key/value pair is compressed 
independently, often wasting space on per-object headers, redundant Huffman tables, and 
repeated symbol statistics. That approach is fine for large blobs, but ineffective for workloads
with millions of small-to-medium, structurally similar objects.

Most modern compressors — including Zstandard — are based on the LZ77 family of algorithms.
They work by replacing repeated sequences in the input with back-references to earlier
occurrences within a sliding window. When each object is compressed separately, the window
is limited to that object’s own content, so the compressor can only exploit patterns that
repeat within the same value. **All cross-object redundancy is lost**.

Dictionary compression changes the game: instead of a small, local window, the compressor
has access to a shared, pre-trained dictionary containing common byte sequences found across
many objects. Each new value can now encode long back-references into this large shared base,
reusing the same token and Huffman tables for the entire workload. The result is far fewer
literal bytes and much higher compression ratios — especially for small JSON, text, or
message-like objects that share similar structure.

In short, a client-side compressor can only see inside a single message, while MCDC’s server-side
dictionary compression sees across the entire dataset. That global view enables back-references 
to patterns every client shares — a capability fundamentally impossible when compressing each 
item in isolation.

**Dictionary compression** fixes that inefficiency:

1. **Training** — MCDC collects samples of your workload and trains a shared Zstandard dictionary.  
2. **Encoding** — New values are compressed using that dictionary; redundant structure vanishes.  
3. **Adapting** — The dictionary is periodically retrained as data evolves.

The result is smaller objects, lower memory usage, and less network traffic —  without touching 
your application code or client libraries.

⸻

## 🧠 Facebook Managed Compression

The first adopter — and arguably the inventor — of this approach was the Facebook Engineering Team.
They introduced it under the name “Managed Compression” in a 2018 engineering blog post:

👉 [5 Ways Facebook Improved Compression at Scale with Zstandard](https://engineering.fb.com/2018/12/19/core-infra/zstandard/)

> “One important use case for managed compression is compressing values in caches.
> Traditional compression is not very effective in caches, since each value must be compressed individually.
> Managed compression exploits knowledge of the structure of the key (when available) to intelligently group values into self-similar categories.
> It then trains and ships dictionaries for the highest-volume categories.
> The gains vary by use case but can be significant.
> Our caches can store up to 40 percent more data using the same hardware.”
— Facebook Engineering Blog, 2018

⸻

## 🔍 How MCDC Differs

While Facebook’s *Managed Compression* was a pioneering concept, **MCDC** takes the idea further — making it fully self-contained and open-source.

| **Aspect** | **Facebook Managed Compression** | **MCDC (Memcached Dictionary Compression)** |
|-------------|----------------------------------|---------------------------------------------|
| Architecture | Centralized *Managed Compression Service* | Integrated entirely inside the caching server |
| Dictionary Training | Performed externally by a centralized service | Done automatically inside MCDC at runtime |
| Monitoring | Centralized telemetry and workload tracking | Local, adaptive workload analysis |
| Dictionary Lifecycle | Pushed periodically from a central service | Trained, validated, and rotated on the fly |
| Availability | Proprietary (internal to Facebook) | 100% open-source |
| Deployment Complexity | Requires coordination with an external system | Drop-in replacement for Memcached — no dependencies |


⸻

🧩 Why It Matters

By embedding training, monitoring, and dictionary management directly inside the cache, MCDC eliminates the need for any external coordination service — achieving
the benefits of Managed Compression with zero infrastructure overhead.

In short:

> Facebook invented it, MCDC democratized it.

⸻

📈 Real-World Impact

Real-world deployments have shown that dictionary-based compression dramatically improves cache efficiency — especially for workloads dominated by small and repetitive objects (e.g. JSONs, tweets, logs, or structured metadata).

🚀 What We Know from Industry
- Facebook reported up to 40 % more data stored on the same hardware using managed compression.
- Their gains were consistent across multiple caching tiers and data types — demonstrating that even modest redundancy across keys can yield major savings once a shared dictionary is applied.

🧮 What to Expect from MCDC

MCDC integrates this idea directly into the caching layer, so you can expect:
- 1.5 × – 2.5 × memory savings on typical web-scale datasets
- Negligible CPU overhead thanks to the use of Zstandard’s dictionary mode and adaptive training heuristics
- Automatic adaptation — as your workload evolves, MCDC retrains dictionaries and updates them without downtime
- Compression ratios that outperform any client-side compression, because MCDC compresses across similar objects, not just within each value

⸻

## ✨ Key Features

- 🔹 **Drop-in replacement** — fully compatible with Memcached 1.6.x (its a fork of 1.6.38) 
- 🔹 **Zstandard dictionary compression** with automatic retraining  
- 🔹 **Dynamic sampling** and adaptive dictionary updates  
- 🔹 **Namespace-aware** compression and statistics  
- 🔹 **Low CPU overhead** — typical impact under 20-30 % at 2× memory savings  
- 🔹 **JSON-based configuration and introspection commands**  
- 🔹 **Zero client changes, no proxy required**

---

## 🧭 New Commands

| Command | Description |
|----------|-------------|
| `mcz config [json]` | Prints current MCDC configuration |
| `mcz stats [namespace] [json]` | Returns detailed compression and memory statistics global and per-namespace|
| `mcz ns` | Lists active namespaces and their dictionaries |
| `mcz reload [json]` | Forces dictionaries reload |
| `mcz sampler [start stop status]` | Controls data spooling (for offline training) |

All new commands follow the Memcached text protocol and can be tested via `telnet` or `nc`.

---

## 🧱 Prerequisites

You need standard Memcached build dependencies plus Zstandard.

Supported toolchains:
-	GCC 11+  / Clang 14+
-	libevent ≥ 2.1, libzstd ≥ 1.5

Tested on Linux (x86-64 / aarch64) and macOS (arm64 / x86-64)

⸻

## ⚙️ Build

```
git clone https://github.com/VladRodionov/mcdc.git
cd mcdc
git checkout mcdc
```

Linux(Ubuntu):
```bash
sudo apt-get update
sudo apt-get install -y build-essential autotools-dev pkgconf autoconf automake libtool m4 autoconf-archive \
    libevent-dev libzstd-dev
./autogen.sh
./configure --with-zstd --with-libevent
make
```

MacOS - use `build-mac.sh`

You’ll get:
```
memcached         # optimized release build + dictionary compression
memcached-debug   # debug
```

Example startup with dictionary compression enabled:

```
./memcached -m 3000 -z mcz.conf -p 11211
```

Example mcz.conf:

```
enable_comp=true
enable_dict=true
dict_dir=../dict-data
dict_size=1M
comp_level=3
min_comp_size=32
max_comp_size=256K

enable_training=true
retraining_interval=2h
min_training_size=0
ewma_alpha=0.20
retrain_drop=0.12
train_mode=fast
dict_retain_max=8

enable_sampling=true
sample_p=0.02
sample_window_duration=100
spool_dir=./samples
spool_max_bytes=64MB
```
For deployment, monitoring, and systemd service integration, see the official Memcached installation guide.
MCDC follows the same conventions and CLI options.

⸻

## 📊 Benchmarks (Coming Soon)

This section will include reproducible membench results comparing
vanilla Memcached 1.6.38 vs MCDC under multiple datasets
(tweets, JSON objects, and mixed workloads).

Stay tuned — benchmark graphs and metrics will be published here soon.

⸻

## 😂 Funny and Curious Moments During Development

Developing MCDC wasn’t just compression ratios and profiler traces — there were some fun surprises:
- The Ancient Perl Tests:
Memcached still relies on a large suite of Perl regression tests.
Running them in 2025 felt like archaeology — proof that some code truly never dies.
- The “Random Data” Paradox:
While verifying compression behavior, I discovered that
even random-looking sequences can compress efficiently… if the alphabet is small (e.g., A–Z).
Fewer than 256 unique symbols = exploitable structure for Zstandard’s entropy coder.
- Wrong assumptions
Perl scripts assumed  that data is immutable, MCDC’s dictionary blew those expectations apart — 
even Perl was surprised!

These moments reminded me that data “randomness” is often an illusion — and that good compression 
still finds patterns in places humans don’t.

⸻

## 📜 License

MCDC is released under the Apache 2.0 License.
It includes and extends Memcached 1.6.38, which is distributed under a BSD-style license.
All trademarks and copyrights remain with their respective owners.

⸻

## 🙌 Acknowledgements

MCDC builds upon two decades of exceptional work by the Memcached open-source community.
Special thanks to the maintainers for keeping Memcached simple, fast, and stable —
making it a perfect foundation for further innovation.

Developed with ❤️ by Vlad Rodionov vladrodionov@gmail.com

My other projects: [Carrot Data](https://www.github.com/carrotdata)

“Cache Smart, Save More.”

---

