### MPSC: throughput (million items/sec, median)

| producers | MutexQueue | MpscQueue | BoundedMpmcQueue |
|---|---|---|---|
| 1 | 30.2 | 14.2 | 96.7 |
| 2 | 15.2 | 26.1 | 21.7 |
| 4 | 9.5 | 15.5 | 6.8 |
| 6 | 7.2 | 16.1 | 4.3 |
| 7 | 6.5 | 16.0 | 4.6 |

### MPSC: latency at a fixed offered load (median of runs)

| producers | load | queue | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| 1 | 3.55 M/s | MutexQueue | 9.2 µs | 64.5 µs | 151.6 µs |
| 1 | 3.55 M/s | MpscQueue | 203 ns | 3.1 ms | 3.9 ms |
| 1 | 3.55 M/s | BoundedMpmcQueue | 111 ns | 543 ns | 20.5 µs |
| 2 | 3.81 M/s | MutexQueue | 17.9 µs | 327.7 µs | 1.0 ms |
| 2 | 3.81 M/s | MpscQueue | 251 ns | 5.2 µs | 34.8 µs |
| 2 | 3.81 M/s | BoundedMpmcQueue | 195 ns | 719 ns | 24.1 µs |
| 4 | 1.71 M/s | MutexQueue | 32.8 µs | 2.4 ms | 2.7 ms |
| 4 | 1.71 M/s | MpscQueue | 735 ns | 1.1 µs | 28.2 µs |
| 4 | 1.71 M/s | BoundedMpmcQueue | 407 ns | 863 ns | 24.1 µs |
| 6 | 1.07 M/s | MutexQueue | 27.1 µs | 3.9 ms | 5.4 ms |
| 6 | 1.07 M/s | MpscQueue | 815 ns | 1.2 µs | 84.0 µs |
| 6 | 1.07 M/s | BoundedMpmcQueue | 439 ns | 1.4 µs | 54.3 µs |
| 7 | 1.15 M/s | MutexQueue | 14.6 µs | 2.1 ms | 2.9 ms |
| 7 | 1.15 M/s | MpscQueue | 799 ns | 9.7 µs | 167.9 µs |
| 7 | 1.15 M/s | BoundedMpmcQueue | 431 ns | 3.0 µs | 104.4 µs |

### SPSC (1 producer, 1 consumer)

| capacity | queue | throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| 1024 | MutexQueue | 27.1 M/s | 12.8 µs | 65.5 µs | 118.8 µs |
| 1024 | NaiveSpscQueue | 32.6 M/s | 115 ns | 639.0 µs | 999.4 µs |
| 1024 | SpscQueue | 32.0 M/s | 109 ns | 213.0 µs | 483.3 µs |
| 65536 | MutexQueue | 30.3 M/s | 7.7 µs | 53.2 µs | 122.9 µs |
| 65536 | NaiveSpscQueue | 11.3 M/s | 147 ns | 1.7 µs | 16.4 µs |
| 65536 | SpscQueue | 10.6 M/s | 109 ns | 2.4 µs | 15.4 µs |

### MPMC (BoundedMpmcQueue vs mutex)

| P x C | queue | throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| 1x1 | MutexQueue | 29.8 M/s | 12.8 µs | 67.6 µs | 143.4 µs |
| 1x1 | BoundedMpmcQueue | 118.7 M/s | 107 ns | 607 ns | 26.1 µs |
| 1x2 | MutexQueue | 12.4 M/s | 30.7 µs | 184.3 µs | 270.3 µs |
| 1x2 | BoundedMpmcQueue | 15.8 M/s | 163 ns | 575 ns | 18.9 µs |
| 1x4 | MutexQueue | 9.2 M/s | 24.1 µs | 163.8 µs | 249.9 µs |
| 1x4 | BoundedMpmcQueue | 7.1 M/s | 367 ns | 559 ns | 12.3 µs |
| 2x1 | MutexQueue | 14.7 M/s | 16.1 µs | 88.1 µs | 139.3 µs |
| 2x1 | BoundedMpmcQueue | 20.7 M/s | 163 ns | 671 ns | 23.6 µs |
| 2x2 | MutexQueue | 12.4 M/s | 10.2 µs | 77.8 µs | 127.0 µs |
| 2x2 | BoundedMpmcQueue | 16.6 M/s | 391 ns | 671 ns | 17.9 µs |
| 2x4 | MutexQueue | 13.0 M/s | 22.0 µs | 241.7 µs | 868.4 µs |
| 2x4 | BoundedMpmcQueue | 6.9 M/s | 511 ns | 655 ns | 4.7 µs |
| 4x1 | MutexQueue | 9.5 M/s | 24.1 µs | 4.5 ms | 4.8 ms |
| 4x1 | BoundedMpmcQueue | 6.6 M/s | 495 ns | 1.2 µs | 13.6 µs |
| 4x2 | MutexQueue | 14.1 M/s | 21.5 µs | 94.2 µs | 143.4 µs |
| 4x2 | BoundedMpmcQueue | 8.2 M/s | 447 ns | 703 ns | 16.4 µs |
| 4x4 | MutexQueue | 21.6 M/s | 18.4 µs | 671.7 µs | 1.4 ms |
| 4x4 | BoundedMpmcQueue | 6.6 M/s | 527 ns | 3.5 µs | 475.1 µs |

### Stacks (million ops/sec, median)

| threads | MutexStack | Treiber+HP | Treiber+EBR |
|---|---|---|---|
| 1 | 99.4 | 66.8 | 63.6 |
| 2 | 63.3 | 21.4 | 31.9 |
| 4 | 30.3 | 13.8 | 11.6 |
| 8 | 46.0 | 5.9 | 4.4 |

