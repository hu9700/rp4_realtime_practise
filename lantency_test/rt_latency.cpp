// rt_latency.cpp
// Measures periodic wakeup latency (ns) for a high-priority thread.
// Compile: g++ -O2 -std=c++17 rt_latency.cpp -o rt_latency -pthread
// Run: sudo ./rt_latency [period_us] [iterations] [outfile]
// Example: sudo ./rt_latency 1000 200000 latencies.csv

#include <bits/stdc++.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

using namespace std;

static inline long long timespec_to_ns(const struct timespec &t) {
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static inline void ns_to_timespec(long long ns, struct timespec &t) {
    t.tv_sec = ns / 1000000000LL;
    t.tv_nsec = ns % 1000000000LL;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <period_us> <iterations> <out.csv>\n", argv[0]);
        return 1;
    }
    const long period_us = atol(argv[1]);
    const int iterations = atoi(argv[2]);
    const char* outfn = argv[3];

    // Lock memory to avoid page faults
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        // continue anyway
    }

    // Set CPU affinity to CPU 0
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        perror("sched_setaffinity");
        // continue anyway
    }

    // Set SCHED_FIFO priority high (needs root or CAP_SYS_NICE)
    struct sched_param sp;
    sp.sched_priority = 80;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        perror("sched_setscheduler");
        fprintf(stderr, "Warning: couldn't set SCHED_FIFO. Run as root or with CAP_SYS_NICE to get real-time priority.\n");
    }

    // Pre-touch memory vector to avoid page faults later
    vector<long long> lat_ns;
    lat_ns.reserve(iterations);

    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    long long start_ns = timespec_to_ns(t);
    long long period_ns = period_us * 1000LL;

    long long next_ns = start_ns + period_ns;

    for (int i = 0; i < iterations; ++i) {
        ns_to_timespec(next_ns, t);

        int r;
        // wait until absolute time next_ns
        do {
            r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        } while (r == EINTR);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long now_ns = timespec_to_ns(now);

        long long latency = now_ns - next_ns; // positive if woke late, negative if early
        lat_ns.push_back(latency);

        next_ns += period_ns;
    }

    // Write CSV (header + latencies in ns)
    FILE* f = fopen(outfn, "w");
    if (!f) { perror("fopen"); return 1; }
    fprintf(f, "index,latency_ns\n");
    for (size_t i = 0; i < lat_ns.size(); ++i) {
        fprintf(f, "%zu,%lld\n", i, lat_ns[i]);
    }
    fclose(f);

    // Print simple stats to stdout
    long long minv=LLONG_MAX, maxv=LLONG_MIN, sum=0;
    for (auto v: lat_ns) {
        if (v < minv) minv = v;
        if (v > maxv) maxv = v;
        sum += v;
    }
    double mean = (double)sum / lat_ns.size();
    double var = 0;
    for (auto v: lat_ns) var += (v - mean)*(v - mean);
    var /= lat_ns.size();
    double sd = sqrt(var);

    printf("period_us=%ld iterations=%d\n", period_us, iterations);
    printf("min=%lld ns  max=%lld ns  mean=%.2f ns  sd=%.2f ns\n",
           minv, maxv, mean, sd);
    printf("Wrote %zu samples to %s\n", lat_ns.size(), outfn);

    return 0;
}
