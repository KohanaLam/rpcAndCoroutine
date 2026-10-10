#ifndef KRPC_BENCHMARK_STATS_H
#define KRPC_BENCHMARK_STATS_H

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <time.h>
#include <vector>

inline int64_t ProcessCpuNowNs()
{
    timespec value{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
    return static_cast<int64_t>(value.tv_sec) * 1000000000LL + value.tv_nsec;
}

inline int ReadProcessThreadCount()
{
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key)
    {
        if (key == "Threads:")
        {
            int count = 0;
            status >> count;
            return count;
        }
        std::string rest;
        std::getline(status, rest);
    }
    return 0;
}

inline double PercentileMs(const std::vector<std::vector<int64_t>> &groups, double percentile)
{
    size_t count = 0;
    for (const auto &group : groups)
    {
        count += group.size();
    }

    if (count == 0)
    {
        return 0.0;
    }

    std::vector<int64_t> samples;
    samples.reserve(count);
    for (const auto &group : groups)
    {
        samples.insert(samples.end(), group.begin(), group.end());
    }

    const size_t index = static_cast<size_t>(percentile * (samples.size() - 1));
    std::nth_element(samples.begin(), samples.begin() + index, samples.end());
    return samples[index] / 1000000.0;
}

#endif
