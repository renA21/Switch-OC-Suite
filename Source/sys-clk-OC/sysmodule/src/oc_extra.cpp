#include "oc_extra.h"

CpuCoreUtil::CpuCoreUtil(int coreid = -2, uint64_t ns = 1000'000ULL)
    : m_core_id(coreid), m_wait_time_ns(ns) { }

uint32_t CpuCoreUtil::Get() {
    struct _ctx {
        uint64_t timestamp;
        uint64_t idletick;
    } begin, end;

    begin.timestamp = armTicksToNs(armGetSystemTick());
    begin.idletick = GetIdleTickCount();

    svcSleepThread(m_wait_time_ns);

    end.timestamp = armTicksToNs(armGetSystemTick());
    end.idletick = GetIdleTickCount();

    uint64_t diff_idletick = end.idletick - begin.idletick;
    uint64_t real_elapsed_ns = end.timestamp - begin.timestamp;
    return UTIL_MAX - diff_idletick * 10 * 1000'000ULL / (TICKS_PER_MS * real_elapsed_ns);
}

uint64_t CpuCoreUtil::GetIdleTickCount() {
    uint64_t idletick = 0;
    svcGetInfo(&idletick, InfoType_IdleTickCount, INVALID_HANDLE, m_core_id);
    return idletick;
}


GpuCoreUtil::GpuCoreUtil(uint32_t nvgpu_field)
    : m_nvgpu_field(nvgpu_field) { }

uint32_t GpuCoreUtil::Get() {
    uint32_t load;
    nvIoctl(m_nvgpu_field, NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD, &load);
    return load;
}


ReverseNXSync::ReverseNXSync()
    : m_rt_mode(ReverseNX_NotFound), m_tool_mode(ReverseNX_NotFound) {
    FILE *fp = fopen("/atmosphere/contents/0000000000534C56/flags/boot2.flag", "r");
    m_tool_enabled = fp ? true : false;
    if (fp)
        fclose(fp);
}

SysClkProfile ReverseNXSync::GetProfile(SysClkProfile real) {
    switch (this->GetMode()) {
        case ReverseNX_Docked:
            return SysClkProfile_Docked;
        case ReverseNX_Handheld:
            if (real == SysClkProfile_Docked)
                return SysClkProfile_HandheldChargingOfficial;
        default:
            return real;
    }
}

ReverseNXMode ReverseNXSync::GetMode() {
    if (!this->m_sync_enabled)
        return ReverseNX_NotFound;
    if (this->m_rt_mode)
        return this->m_rt_mode;
    return this->m_tool_mode;
}

ReverseNXMode ReverseNXSync::GetToolModeFromPatch(const char* patch_path) {
    constexpr uint32_t DOCKED_MAGIC = 0x320003E0;
    constexpr uint32_t HANDHELD_MAGIC = 0x52A00000;
    FILE *fp = fopen(patch_path, "rb");
    if (fp) {
        uint32_t buf = 0;
        fread(&buf, sizeof(buf), 1, fp);
        fclose(fp);

        if (buf == DOCKED_MAGIC)
            return ReverseNX_Docked;
        if (buf == HANDHELD_MAGIC)
            return ReverseNX_Handheld;
    }

    return ReverseNX_NotFound;
}

ReverseNXMode ReverseNXSync::RecheckToolMode() {
    ReverseNXMode mode = ReverseNX_NotFound;
    if (this->m_tool_enabled) {
        const char* fileName = "_ZN2nn2oe16GetOperationModeEv.asm64"; // or _ZN2nn2oe18GetPerformanceModeEv.asm64
        const char* filePath = new char[72];
        /* Check per-game patch */
        snprintf((char*)filePath, 72, "/SaltySD/patches/%016lX/%s", this->m_app_id, fileName);
        mode = this->GetToolModeFromPatch(filePath);
        if (!mode) {
            /* Check global patch */
            snprintf((char*)filePath, 72, "/SaltySD/patches/%s", fileName);
            mode = this->GetToolModeFromPatch(filePath);
        }
        delete[] filePath;
    }

    return mode;
}


void PsmExt::ChargingHandler(bool fastChargingEnabled, uint32_t chargingLimit) {
    PsmChargeInfo* info = new PsmChargeInfo;
    Service* session = psmGetServiceSession();
    serviceDispatchOut(session, Psm_GetBatteryChargeInfoFields, *info);

    if (PsmIsFastChargingEnabled(info) != fastChargingEnabled)
        serviceDispatch(session, fastChargingEnabled ? Psm_EnableFastBatteryCharging : Psm_DisableFastBatteryCharging);

    if (PsmIsChargerConnected(info)) {
        u32 chargeNow = 0;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&chargeNow))) {
            bool isCharging = PsmIsCharging(info);
            if (isCharging && chargingLimit < chargeNow)
                serviceDispatch(session, Psm_DisableBatteryCharging);
            if (!isCharging && chargingLimit > chargeNow)
                serviceDispatch(session, Psm_EnableBatteryCharging);
        }
    }

    delete info;
}


Governor::Governor() {
    memset(reinterpret_cast<void*>(&m_cpu_freq), 0, sizeof(m_cpu_freq));
    memset(reinterpret_cast<void*>(&m_gpu_freq), 0, sizeof(m_gpu_freq));

    m_cpu_freq.module = SysClkModule_CPU;
    m_gpu_freq.module = SysClkModule_GPU;

    m_cpu_freq.hz_list = GetTable(SysClkModule_CPU);
    m_gpu_freq.hz_list = GetTable(SysClkModule_GPU);

    m_cpu_freq.boost_hz = Clocks::boostCpuFreq;
    m_cpu_freq.utilref_hz = 2397'000'000;

    m_gpu_freq.boost_hz = 76'800'000;
    m_gpu_freq.min_hz = 153'600'000;
    m_gpu_freq.utilref_hz = 1305'600'000;

    nvInitialize();
    Result rc = nvOpen(&m_nvgpu_field, "/dev/nvhost-ctrl-gpu");
    if (R_FAILED(rc)) {
        ASSERT_RESULT_OK(rc, "nvOpen");
        nvExit();
    }
}

Governor::~Governor() {
    Stop();
    nvClose(m_nvgpu_field);
    nvExit();
}

void Governor::Start() {
    if (m_running)
        return;

    m_running = true;
    Result rc = 0;
    for (int core = 0; core < CORE_NUMS; core++) {
        s_CoreContext* s = &m_cpu_core_ctx[core];
        s->self = this;
        s->id = core;
        int prio = (core == CORE_NUMS - 1) ? 0x3F : 0x3B; // Pre-emptive MT
        rc = threadCreate(&m_t_cpuworker[core], &CpuUtilWorker, (void*)s, NULL, 0x1000, prio, core);
        ASSERT_RESULT_OK(rc, "threadCreate");
        rc = threadStart(&m_t_cpuworker[core]);
        ASSERT_RESULT_OK(rc, "threadStart");
    }
    rc = threadCreate(&m_t_main, &Main, (void*)this, NULL, 0x1000, 0x3F, 3);
    ASSERT_RESULT_OK(rc, "threadCreate");
    rc = threadStart(&m_t_main);
    ASSERT_RESULT_OK(rc, "threadStart");
}

void Governor::Stop() {
    if (!m_running)
        return;

    m_running = false;
    svcSleepThread(TICK_TIME_NS);

    threadWaitForExit(&m_t_main);
    threadClose(&m_t_main);

    for (int core = 0; core < CORE_NUMS; core++) {
        threadWaitForExit(&m_t_cpuworker[core]);
        threadClose(&m_t_cpuworker[core]);
    }
}

void Governor::SetMaxHz(uint32_t max_hz, SysClkModule module) {
    if (!max_hz) // Fallback to apm configuration
        max_hz = Clocks::GetStockClock(m_apm_conf, (SysClkModule)module);

    switch (module) {
        case SysClkModule_CPU:
            m_cpu_freq.max_hz = max_hz;
            break;
        case SysClkModule_GPU:
            m_gpu_freq.max_hz = max_hz;
            m_gpu_freq.min_hz = (m_gpu_freq.max_hz <= 153'600'000) ? max_hz : 153'600'000;
            break;
        case SysClkModule_MEM:
            m_mem_freq = max_hz;
            Clocks::SetHz(SysClkModule_MEM, max_hz);
            break;
        default:
            break;
    }
}

void Governor::SetPerfConf(uint32_t id) {
    m_perf_conf_id = id;
    m_apm_conf = Clocks::GetEmbeddedApmConfig(id);
}

uint32_t Governor::s_FreqContext::GetNormalizedUtil(uint32_t raw_util) {
    return ((uint64_t)raw_util * target_hz / utilref_hz);
}

// Schedutil: https://github.com/torvalds/linux/blob/master/kernel/sched/cpufreq_schedutil.c
// C = 1.25, tipping-point 80.0% (used in Linux schedutil), 1.25 -> 1 + (1 >> 2)
// C = 1.5,  tipping-point 66.7%, 1.5 -> 1 + (1 >> 1)
// Utilization is frequency-invariant (normalized):
//   next_freq = C * max_freq(ref_freq) * util / max
void Governor::s_FreqContext::SetNextFreq(uint32_t norm_util) {
    uint32_t prev_hz = target_hz;
    uint32_t next_freq = (uint64_t)(norm_util + (norm_util >> 1)) * utilref_hz / UTIL_MAX;

    uint32_t adj_next_freq = target_hz;
    if (next_freq > max_hz) {
        adj_next_freq = max_hz;
    } else if (next_freq < min_hz) {
        adj_next_freq = min_hz;
    } else {
        uint32_t* p = hz_list;
        do {
            if (*p >= next_freq) {
                adj_next_freq = *p;
                break;
            }
        } while (*p++);
    }

    target_hz = adj_next_freq;
    bool changed = target_hz != prev_hz;
    if (changed)
        SetHz();
}

void Governor::s_FreqContext::SetHz() {
    if (target_hz)
        Clocks::SetHz(module, target_hz);
}

void Governor::s_FreqContext::SetBoostHz() {
    target_hz = boost_hz;
    if (module == SysClkModule_CPU && max_hz > boost_hz)
        target_hz = max_hz;
    SetHz();
}

void Governor::CpuUtilWorker(void* args) {
    s_CoreContext* s = static_cast<s_CoreContext*>(args);
    int coreid = s->id;
    constexpr int SYS_CORE_ID = (CORE_NUMS - 1);
    Governor* self = s->self;

    while (self->m_running) {
        bool CPUBoosted = apmExtIsCPUBoosted(self->m_perf_conf_id);
        if (CPUBoosted) {
            svcSleepThread(TICK_TIME_NS);
            continue;
        }

        uint64_t timestamp = armTicksToNs(armGetSystemTick());
        s->timestamp = timestamp;
        for (int id = 0; id < CORE_NUMS; id++) {
            if (abs(self->m_cpu_core_ctx[id].timestamp - timestamp) < TICK_TIME_NS * 10)
                continue;

            if (id == SYS_CORE_ID) {
                self->m_cpu_freq.SetBoostHz();
            } else {
                self->m_cpu_freq.target_hz = self->m_cpu_freq.max_hz;
                self->m_cpu_freq.SetHz();
            }
            break;
        }

        s->util = self->m_cpu_freq.GetNormalizedUtil(CpuCoreUtil(coreid, TICK_TIME_NS).Get());
    }
}

void Governor::Main(void* args) {
    Governor* self = static_cast<Governor*>(args);
    s_FreqContext* cpu_ctx = &self->m_cpu_freq;
    s_FreqContext* gpu_ctx = &self->m_gpu_freq;
    uint32_t nvgpu_field = self->m_nvgpu_field;

    s_Util cpu_util, gpu_util;
    auto GetAdjCpuUtil = [self, cpu_util]() mutable {
        uint64_t util = self->m_cpu_core_ctx[0].util;
        for (size_t i = 1; i < CORE_NUMS; i++) {
            if (util < self->m_cpu_core_ctx[i].util)
                util = self->m_cpu_core_ctx[i].util;
        }
        cpu_util.Update(util);
        return cpu_util.Get();
    };

    auto GetAdjGpuUtil = [gpu_ctx, nvgpu_field, gpu_util]() mutable {
        uint32_t util = gpu_ctx->GetNormalizedUtil(GpuCoreUtil(nvgpu_field).Get());
        gpu_util.Update(util);
        return gpu_util.Get();
    };

    constexpr uint64_t UPDATE_CONTEXT_RATE = SAMPLE_RATE / 2;
    uint64_t update_ticks = UPDATE_CONTEXT_RATE;
    bool CPUBoosted = false;
    bool GPUThrottled = false;

    while (self->m_running) {
        bool shouldUpdateContext = update_ticks++ >= UPDATE_CONTEXT_RATE;
        if (shouldUpdateContext) {
            update_ticks = 0;
            uint32_t hz = Clocks::GetCurrentHz(SysClkModule_GPU);
            // Sleep mode detected, wait 10 ticks
            while (!hz) {
                svcSleepThread(10 * TICK_TIME_NS);
                hz = Clocks::GetCurrentHz(SysClkModule_GPU);
            }

            GPUThrottled = apmExtIsBoostMode(self->m_perf_conf_id);
            CPUBoosted = apmExtIsCPUBoosted(self->m_perf_conf_id);

            gpu_ctx->target_hz = hz;
            if (GPUThrottled)
                gpu_ctx->SetBoostHz();

            hz = Clocks::GetCurrentHz(SysClkModule_CPU);
            cpu_ctx->target_hz = hz;
            if (CPUBoosted)
                cpu_ctx->SetBoostHz();

            hz = Clocks::GetCurrentHz(SysClkModule_MEM);
            if (!self->m_mem_freq)
                self->m_mem_freq = hz;
            if (hz != self->m_mem_freq)
                Clocks::SetHz(SysClkModule_MEM, self->m_mem_freq);
        }

        if (!GPUThrottled)
            gpu_ctx->SetNextFreq(GetAdjGpuUtil());
        if (!CPUBoosted)
            cpu_ctx->SetNextFreq(GetAdjCpuUtil());

        svcSleepThread(TICK_TIME_NS);
    }
}

