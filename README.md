[**Documentation**](https://github.com/VladRodionov/mcdc/wiki/Quick-Start-Guide) ⚡⚡⚡ [**Forum**](https://github.com/VladRodionov/mcdc/discussions)

# MC/DC — Memory Cache with Dictionary Compression

⚡ **For Those About to Cache (We Salute You!)** 🤘🎸

**MC/DC** (Memory Cache with Dictionary Compression) is a **drop-in replacement for Memcached 1.6.38+** with built-in 
**Zstandard dictionary compression**, delivering up to 1.5×–2.5× better RAM efficiency without any client-side 
changes. MC/DC is developed and maintained by [Carrot Data](https://github.com/carrotdata) — a project focused on practical, 
SSD-friendly, memory-efficient caching technologies.

## What Is MC/DC?

MC/DC **extends Memcached** with adaptive, server-side data compression.  Instead of compressing 
each object individually (like client SDKs usually do), MC/DC automatically learns shared byte 
patterns across your data and uses them  to build a **Zstandard dictionary**. Once trained, 
the dictionary allows ultra-compact compression of small-to-medium, structurally similar objects — 
tweets, JSON fragments, log entries, etc. — at minimal CPU cost.

All standard Memcached commands, clients, and SDKs continue to work unchanged.

## Dictionary Compression — Why It Matters

Typical “client-side compression” operates in isolation: every key/value pair is compressed 
independently, often wasting space on per-object headers, redundant Huffman tables, and 
repeated symbol statistics. That approach is fine for large blobs, but ineffective for workloads
with millions of small-to-medium, structurally similar objects.

Most modern compressors — including Zstandard — are based on the **LZ77** family of algorithms.
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

In short, a client-side compressor can only see inside a single message, while MC/DC’s server-side
dictionary compression sees across the entire dataset. That global view enables back-references 
to patterns every client shares — **a capability fundamentally impossible when compressing each 
item in isolation**.

**Dictionary compression** fixes that inefficiency:

1. **Training** — MC/DC collects samples of your workload and trains a shared Zstandard dictionary.  
2. **Encoding** — New values are compressed using that dictionary; redundant structure vanishes.  
3. **Adapting** — The dictionary is periodically retrained as data evolves.

The result is smaller objects, lower memory usage, and less network traffic —  without touching 
your application code or client libraries.

## Facebook Managed Compression

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

In MC/DC these "self-similar" categories are called "namespaces". See [Quick Start Guide](https://github.com/VladRodionov/mcdc/wiki/Quick-Start-Guide) for more information.

## How MC/DC Differs

While Facebook’s *Managed Compression* was a pioneering concept, **MC/DC** takes the idea further — making it fully self-contained and open-source.

| **Aspect** | **Facebook Managed Compression** | **MC/DC** |
|-------------|----------------------------------|---------------------------------------------|
| Architecture | Centralized *Managed Compression Service* | Integrated entirely inside the caching server |
| Dictionary Training | Performed externally by a centralized service | Done automatically inside MC/DC at runtime |
| Monitoring | Centralized telemetry and workload tracking | Local, adaptive workload analysis |
| Dictionary Lifecycle | Pushed periodically from a central service | Trained, validated, and rotated on the fly, external dictionaries are supported as well |
| Availability | Proprietary (internal to Facebook) | 100% open-source |
| Deployment Complexity | Requires coordination with an external system | Drop-in replacement for Memcached — no dependencies |

### Why It Matters

By embedding training, monitoring, and dictionary management directly inside the cache, MC/DC eliminates the need for any external coordination service — achieving
the benefits of Managed Compression with zero infrastructure overhead.

In short:

> Facebook invented it, MC/DC democratized it.

### Real-World Impact

Real-world deployments have shown that dictionary-based compression dramatically improves cache efficiency — especially for workloads dominated by small and repetitive objects (e.g. JSONs, tweets, logs, or structured metadata).

What We Know from Industry
- Facebook reported up to 40 % more data stored on the same hardware using managed compression.
- Their gains were consistent across multiple caching tiers and data types — demonstrating that even modest redundancy across keys can yield major savings once a shared dictionary is applied.

### What to Expect from MC/DC

MC/DC integrates this idea directly into the caching layer, so you can expect:
- up to 1.5 × – 2.0 × memory savings on typical web-scale datasets
- Negligible CPU overhead thanks to the use of Zstandard’s dictionary mode and adaptive training heuristics
- Automatic adaptation — as your workload evolves, MC/DC retrains dictionaries and updates them without downtime
- Compression ratios that outperform any client-side compression, because MC/DC compresses across similar objects, not just within each value

## Key Features

- **Drop-in replacement** — fully compatible with Memcached 1.6.x (its a fork of 1.6.38) 
- **Zstandard dictionary compression** with automatic retraining  
- **Dynamic sampling** and adaptive dictionary updates  
- **Namespace-aware** compression and statistics  
- **Low CPU overhead** — typical impact under 20-30 % (relative to a baseline) at 2× memory savings  
- **JSON-based configuration and introspection commands**  
- **Zero client changes, no proxy required**
- **Text, meta and binary protocols** are supported
- **Extstore** is supported


## New Commands

| Command | Description |
|----------|-------------|
| `mcdc config [json]` | Prints current MC/DC configuration |
| `mcdc stats [namespace] [json]` | Returns detailed compression and memory statistics global and per-namespace|
| `mcdc ns` | Lists active namespaces and their dictionaries |
| `mcdc reload [json]` | Forces dictionaries reload |
| `mcdc sampler [start stop status]` | Controls data spooling (for offline training, cluster analysis for namespace detection) |

All new commands follow the Memcached text protocol and can be tested via `telnet` or `nc`.

## Prerequisites

You need standard Memcached build dependencies plus Zstandard.

Supported toolchains:
-	GCC 11+  / Clang 14+
-	libevent ≥ 2.1, libzstd ≥ 1.5

Tested on Linux (x86-64 / aarch64) and macOS (arm64 / x86-64)

## Build

```
git clone https://github.com/VladRodionov/mcdc.git
cd mcdc
git fetch --all --tags       # Make sure you have all tags
git checkout 1.6.38-mcdc-dev
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

Or if you want to run "vanilla" `memcached`

```
./memcached -m 3000 -o mcdc_disabled -p 11211
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
MC/DC follows the same conventions and CLI options.

## Current limitations

- `append` and `prepend` commands require special handling. These commands are currently ignored by the MC/DC
runtime (no compression is applied). Support for them may be added in the future if there is sufficient demand.

- Meta commands that return only metadata (without values) may temporarily report an incorrect value size —
specifically, the compressed size rather than the original uncompressed one. This is also a known issue and is being
tracked for resolution.

⚠️ Important:
If you use `append` or `prepend`, make sure to issue a `set` or `add` command first, with a value whose size is 
smaller than the minimum compression threshold (`min_comp_size` configuration parameter). You can use zero length value,
memcached supports it. Otherwise, the initial value will be compressed, and subsequent `append` or `prepend` operations 
may corrupt the stored data. This issue is tracked and will be fixed soon. Please let me know if you rely on these 
commands in your workloads.
  
## Benchmarks

This section includes reproducible [**membench**](https://github.com/carrotdata/membench) results comparing vanilla 
Memcached 1.6.38 vs MC/DC under multiple datasets (tweets, JSON objects, and mixed workloads). For detailed descriptions 
of the tests and workloads, please refer to the original benchmark repository (link above).

We compared MC/DC under two configurations:
- Without dictionary compression:
enable_comp=true, enable_dict=false — only standard Zstandard (zstd) compression is used, effectively emulating client-side compression.
- With dictionary compression:
enable_comp=true, enable_dict=true — full dictionary-based compression is enabled, allowing MCDC to automatically train and apply shared dictionaries.

The results are summarized in Table 1 below.

**Table 1. Memory footprint per dataset (lower is better) and relative efficiency gain of MCDC vs Memcached.**

| Server / Dataset            | airbnb | amazon | arxiv | dblp | github | ohio | reddit | spotify | twitter |
|-----------------------------|:------:|:------:|:-----:|:----:|:------:|:----:|:------:|:-------:|:-------:|
| **memcached 1.6.38 (zstd)** |  19.9  |  19.2  | 21.0  | 19.1 |  14.3  | 9.4  |  13.2  |  21.2   |  12.3   |
| **mcdc (zstd+dict)**        |  10.6  |  14.2  | 13.6  | 11.0 |   7.0  | 6.9  |   4.4  |   8.5   |   6.7   |
| **Efficiency gain**         | +88%   | +35%   | +54%  | +74% | +104%  | +36% | +200%  | +149%   |  +84%   |

*Notes:* “Efficiency gain” is computed as **(memcached / mcdc – 1)*100%**.  
Values are in GBs (e.g., memory bytes per dataset); smaller numbers indicate better efficiency.

### Quick analysis

- **Overall:** MCDC typically delivers **~1.8–1.9×** better memory efficiency (geometric mean ≈ **1.85×**).  
- **Best cases:** **reddit (+200%)** and **spotify (+149%)** show the strongest wins—high structural similarity benefits dictionary compression most.  
- **Moderate cases:** **amazon (+35%)** and **ohio (+36%)** still see meaningful gains, indicating some cross-object redundancy.  
- **Averages:** Baseline average = **16.62**, MCDC average = **9.21** → ~**44.6%** absolute reduction in average footprint.

## Funny and Curious Moments During Development

Developing MC/DC wasn’t just compression ratios and profiler traces — there were some fun surprises:
- The Ancient Perl Tests: Memcached still relies on a large suite of Perl regression tests.
Running them in 2025 felt like archaeology — proof that some code truly never dies.
- The “Random Data” Paradox: While verifying compression behavior, I discovered that
even random-looking sequences can compress efficiently… if the alphabet is small (e.g., A–Z).
Fewer than 256 unique symbols = exploitable structure for Zstandard’s entropy coder.
- Wrong assumptions: Perl scripts assumed  that data is immutable, MCDC’s dictionary blew those expectations apart — 
even Perl was surprised!

These moments reminded me that data “randomness” is often an illusion — and that good compression 
still finds patterns in places humans don’t.

## License

MC/DC is released under the Apache 2.0 License. It includes and extends Memcached 1.6.38, which is distributed under a BSD-style license.
All trademarks and copyrights remain with their respective owners.

## Acknowledgements

MC/DC builds upon two decades of exceptional work by the Memcached open-source community. Special thanks to the maintainers for keeping 
Memcached simple, fast, and stable — making it a perfect foundation for further innovation.

Developed with ❤️ by Vlad Rodionov vladrodionov@gmail.com

My other projects: [Carrot Data](https://www.github.com/carrotdata) “Cache Smart, Save More.”

© 2025 Vladimir Rodionov. All rights reserved.


