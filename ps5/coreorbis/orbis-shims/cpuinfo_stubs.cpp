// Fixed Zen2 topology for Orbis (matches poc2: ncpu=16, 64B lines).
// Single cluster: thread-placement code still works, just never migrates.
// PS4 port (AI-assisted): the PS4's two Jaguar modules, 8 cores without SMT (the app runs on 7 of them).
#include "cpuinfo.h"

#ifdef ORBIS_PS4
static constexpr uint32_t kThreadsPerCore = 1;
#else
static constexpr uint32_t kThreadsPerCore = 2;
#endif
static constexpr uint32_t kCores = 8;
static constexpr uint32_t kProcessors = kCores * kThreadsPerCore;

static const struct cpuinfo_cache s_l1i = {64};
static const struct cpuinfo_cache s_l1d = {64};
static const struct cpuinfo_cluster s_cluster = {0, kProcessors, 0, kCores, 0};
static struct cpuinfo_processor s_procs[kProcessors];
static struct cpuinfo_core s_cores[kCores];
static const struct cpuinfo_uarch_info s_uarch = {cpuinfo_uarch_zen2};

extern "C" bool cpuinfo_initialize(void)
{
  for (uint32_t i = 0; i < kProcessors; i++)
  {
    s_procs[i].smt_id = i % kThreadsPerCore;
    s_procs[i].cluster = &s_cluster;
    s_procs[i].cache.l1i = &s_l1i;
    s_procs[i].cache.l1d = &s_l1d;
  }
  for (uint32_t i = 0; i < kCores; i++)
  {
    s_cores[i].processor_start = i * kThreadsPerCore;
    s_cores[i].processor_count = kThreadsPerCore;
    s_cores[i].core_id = i;
    s_cores[i].cluster = &s_cluster;
    s_cores[i].vendor = cpuinfo_vendor_amd;
  }
  return true;
}

extern "C" const struct cpuinfo_processor* cpuinfo_get_processor(uint32_t index)
{
  if (index >= kProcessors)
    return 0;
  return &s_procs[index];
}

extern "C" uint32_t cpuinfo_get_processors_count(void)
{
  return kProcessors;
}

extern "C" uint32_t cpuinfo_get_clusters_count(void)
{
  return 1;
}

extern "C" const struct cpuinfo_cluster* cpuinfo_get_cluster(uint32_t index)
{
  if (index != 0)
    return 0;
  return &s_cluster;
}

extern "C" uint32_t cpuinfo_get_cores_count(void)
{
  return kCores;
}

extern "C" const struct cpuinfo_core* cpuinfo_get_core(uint32_t index)
{
  if (index >= kCores)
    return 0;
  return &s_cores[index];
}

extern "C" uint32_t cpuinfo_get_uarchs_count(void)
{
  return 1;
}

extern "C" const struct cpuinfo_uarch_info* cpuinfo_get_uarch(uint32_t index)
{
  if (index != 0)
    return 0;
  return &s_uarch;
}
