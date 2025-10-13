#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <getopt.h>

#ifndef NO_NVML
#include <nvml.h>
#endif

#include <linux/ipmi.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DCMI_NETFN              0x2c
#define DCMI_GET_POWER_READING  0x02
#define DCMI_GROUP_EXT          0xdc

static unsigned long interval = 1000000; // Default interval in usecs
static unsigned timeout = 120; // Default timeout in seconds
static int has_dram = 0;
static int has_mmdd = 0;
static int has_energy = 0;
static int has_headers = 0;
static int use_ipmi = 0;
#ifndef NO_NVML
static int use_gpu = 0, per_gpu_output = 0;
static int gpu_count;
static nvmlDevice_t* gpu_handle; // Handle for up to 2 GPUs
static double *gpu_powers, *gpu_powers_last, *gpu_energies;
#endif

typedef struct
{
        int fd;
        struct ipmi_addr addr;
} ipmi_context_t;

static ipmi_context_t ipmi_ctx;

static void
usage(void)
{
        printf(
                "Usage: enelog [-i interval] [-t timeout] [-h]\n"
                "Options:\n"
                "  -i <interval>  Sampling interval in seconds (default: 1 second)\n"
                "  -t <timeout>   Total measurement duration in seconds (default: 120 seconds)\n"
                "  -d             Enable DRAM power measurement\n"
                "  -D             Show MM-dd field in outputs\n"
                "  -E             Show energy field in outputs\n"
                "  -H             Show field headers in outputs\n"
                "  -h             Show this help message and exit\n"
                "  -I             Enable IPMI power measurement\n"
#ifndef NO_NVML
                "  -g             Enable GPU power measurement\n"
                "  -G             Show all powers of GPU devices\n"
#endif
        );
}

static int
init_ipmi(ipmi_context_t* ctx)
{
        const char* device_paths[] = {"/dev/ipmi0", "/dev/ipmi/0", "/dev/ipmidev/0", NULL};
        for (int i = 0; device_paths[i] != NULL; i++)
        {
                ctx->fd = open(device_paths[i], O_RDWR);
                if (ctx->fd >= 0)
                {
                        fprintf(stderr, "IPMI device found: %s\n", device_paths[i]);
                        break;
                }
        }

        if (ctx->fd < 0)
        {
                perror("Failed to open IPMI device");
                return -1;
        }

        struct ipmi_system_interface_addr* si_addr = (struct ipmi_system_interface_addr*)&ctx->addr;
        si_addr->addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE;
        si_addr->channel = IPMI_BMC_CHANNEL;
        si_addr->lun = 0;

        return 0;
}

static int
send_ipmi_command(ipmi_context_t* ctx, uint8_t netfn, uint8_t cmd,
                  uint8_t* req_data, int req_len,
                  uint8_t* resp_data, int* resp_len)
{
        struct ipmi_req req;
        struct ipmi_recv recv;
        static long msgid = 0;

        memset(&req, 0, sizeof(req));
        req.addr = (unsigned char*)&ctx->addr;
        req.addr_len = sizeof(struct ipmi_system_interface_addr);
        req.msgid = msgid++;
        req.msg.netfn = netfn;
        req.msg.cmd = cmd;
        req.msg.data_len = req_len;
        req.msg.data = req_data;

        if (ioctl(ctx->fd, IPMICTL_SEND_COMMAND, &req) < 0)
        {
                perror("Failed to send IPMI command");
                return -1;
        }

        memset(&recv, 0, sizeof(recv));
        recv.addr = (unsigned char*)&ctx->addr;
        recv.addr_len = sizeof(ctx->addr);
        recv.msg.data = resp_data;
        recv.msg.data_len = *resp_len;

        if (ioctl(ctx->fd, IPMICTL_RECEIVE_MSG_TRUNC, &recv) < 0)
        {
                if (errno != EAGAIN)
                {
                        perror("Failed to receive IPMI response");
                        return -1;
                }
        }

        *resp_len = recv.msg.data_len;

        if (resp_data[0] != 0x00)
        {
                fprintf(stderr, "IPMI Error: completion code = 0x%02x\n", resp_data[0]);
                return -1;
        }

        return 0;
}

static int
get_ipmi_power_reading(ipmi_context_t* ctx)
{
        uint8_t req_data[4] = {DCMI_GROUP_EXT, 0x01, 0x00, 0x00};
        uint8_t resp_data[256];
        int resp_len = sizeof(resp_data);

        if (send_ipmi_command(ctx, DCMI_NETFN, DCMI_GET_POWER_READING,
                              req_data, 4, resp_data, &resp_len) < 0)
        {
                return -1;
        }

        if (resp_len < 16)
        {
                fprintf(stderr, "IPMI response data length is too short: %d\n", resp_len);
                return -1;
        }

        uint16_t current_power = resp_data[2] | (resp_data[3] << 8);

        return current_power;
}

static int
open_powercap(const char* path)
{
        int fd;

        fd = open(path, O_RDONLY);
        if (fd < 0)
        {
                char* errmsg;
                asprintf(&errmsg, "failed to open %s", path);
                perror(errmsg);
                exit(EXIT_FAILURE);
        }
        return fd;
}

static int
open_powercap_pkg0_energy(void)
{
        return open_powercap("/sys/class/powercap/intel-rapl:0/energy_uj");
}

static int
open_powercap_pkg0_dram_energy(void)
{
        for (int i = 0;; i++)
        {
                int fd;
                char *path, name[64];

                asprintf(&path, "/sys/class/powercap/intel-rapl:0:%d/name", i);
                fd = open_powercap(path);
                read(fd, name, sizeof name);
                free(path);
                if (strncmp(name, "dram", 4) == 0)
                {
                        asprintf(&path, "/sys/class/powercap/intel-rapl:0:%d/energy_uj", i);
                        fd = open_powercap(path);
                        free(path);
                        return fd;
                }
        }
}

static double
read_powercap_energy(int fd)
{
        unsigned long energy_uj;
        char buf[128];

        lseek(fd, 0, SEEK_SET);
        if (read(fd, buf, sizeof(buf)) == -1)
        {
                perror("failed to read");
                exit(EXIT_FAILURE);
        }
        energy_uj = strtoul(buf, NULL, 10);
        return (double)energy_uj / 1e6;
}

static void
read_powercap_power_energy(int fd, double* p_power, double* p_energy, double* p_acc_energy_last)
{
        double acc_energy_cur = read_powercap_energy(fd);

        *p_energy = acc_energy_cur - *p_acc_energy_last;
        *p_power = *p_energy / (interval / 1000000);
        *p_acc_energy_last = acc_energy_cur;
}

static inline uint64_t
get_usec_elapsed(struct timespec* start, struct timespec* end)
{
        time_t sec = end->tv_sec - start->tv_sec;
        long nsec = end->tv_nsec - start->tv_nsec;

        if (nsec < 0)
        {
                sec -= 1;
                nsec += 1000000000L;
        }
        return (uint64_t)sec * 1000000 + (uint64_t)(nsec / 1000);
}

static void
setup_current_time_str(char* timebuf)
{
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        if (has_mmdd)
                strftime(timebuf, 32, "%m-%d %H:%M:%S", &tm_now);
        else
                strftime(timebuf, 32, "%H:%M:%S", &tm_now);
}

static void
wait_until_aligned_interval(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);

        unsigned intv = interval / 1000000; // Convert microseconds to seconds
        unsigned sec = ts.tv_sec % 60;
        unsigned nsec = ts.tv_nsec;
        unsigned wait_sec = (sec % intv == 0) ? 0 : intv - (sec % intv);
        long wait_usec = (1000000000L - nsec) / 1000;

        // total wait time(microsecond)
        long total_wait = wait_sec * 1000000L + wait_usec;
        if (total_wait > 0)
                usleep(total_wait);
}

#ifndef NO_NVML

static void
init_nvml(void)
{
        if (nvmlInit_v2() != NVML_SUCCESS)
        {
                fprintf(stderr, "NVML init failed\n");
                exit(EXIT_FAILURE);
        }
        if (nvmlDeviceGetCount_v2(&gpu_count) != NVML_SUCCESS || gpu_count == 0)
        {
                fprintf(stderr, "No NVIDIA GPUs found\n");
                exit(EXIT_FAILURE);
        }
        gpu_handle = (nvmlDevice_t*)malloc(sizeof(nvmlDevice_t) * gpu_count);
        if (!gpu_handle)
        {
                perror("malloc for gpu_handle");
                exit(EXIT_FAILURE);
        }
        gpu_powers = (double*)malloc(sizeof(double) * gpu_count * 2);
        if (!gpu_powers)
        {
                perror("malloc for gpu_powers");
                exit(EXIT_FAILURE);
        }
        gpu_energies = (double*)malloc(sizeof(double) * gpu_count);
        if (!gpu_energies)
        {
                perror("malloc for gpu_energies");
                exit(EXIT_FAILURE);
        }
        for (unsigned int i = 0; i < gpu_count; i++)
        {
                if (nvmlDeviceGetHandleByIndex_v2(i, &gpu_handle[i]) != NVML_SUCCESS)
                {
                        fprintf(stderr, "Failed to get handle for GPU %d\n", i);
                        exit(EXIT_FAILURE);
                }
                gpu_energies[i] = 0.0;
        }
}

static void
shutdown_nvml(void)
{
        if (gpu_handle)
        {
                free(gpu_handle);
                gpu_handle = NULL;
        }
        nvmlShutdown();
}

#define GET_GPU_POWERS_CUR() \
        (gpu_powers_last == NULL || gpu_powers_last != gpu_powers) ? (gpu_powers): (gpu_powers + gpu_count)

static void
read_energy_gpu(void)
{
        double* gpu_powers_cur = GET_GPU_POWERS_CUR();

        for (unsigned int i = 0; i < gpu_count; i++)
        {
                unsigned int milli_watt;
                if (nvmlDeviceGetPowerUsage(gpu_handle[i], &milli_watt) == NVML_SUCCESS)
                        gpu_powers_cur[i] = milli_watt / 1000.0;
                if (gpu_powers_last != NULL)
                        gpu_energies[i] = (gpu_powers_last[i] + gpu_powers_cur[i]) * interval / 1000000.0 / 2;
        }
}

static void
get_total_power_energy_gpu(double* p_total_power, double* p_total_energy)
{
        double* gpu_powers_cur = GET_GPU_POWERS_CUR();

        *p_total_power = 0;
        *p_total_energy = 0;
        for (unsigned int i = 0; i < gpu_count; i++)
        {
                (*p_total_power) += gpu_powers_cur[i];
                (*p_total_energy) += gpu_energies[i];
        }
}

#endif

static void
print_headers(void)
{
        if (!has_headers)
                return;
        printf("#");
        if (has_mmdd)
                printf(" mm-dd");
        printf(" HH:MM:ss CPU(W)");
        if (has_energy)
                printf(" CPU(J)");
        if (has_dram)
        {
                printf(" DRAM(W)");
                if (has_energy)
                        printf(" DRAM(J)");
        }

#ifndef NO_NVML
        if (use_gpu)
        {
                printf(" GPU(W)");
                if (has_energy)
                        printf(" GPU(J)");
                if (per_gpu_output)
                {
                        for (int i = 0; i < gpu_count; i++)
                        {
                                printf(" GPU%02d(W)", i);
                                if (has_energy)
                                        printf(" GPU%02d(J)", i);
                        }
                }
        }
#endif
        if (use_ipmi)
        {
                printf(" IPMI(W)");
        }
        if (has_energy)
                printf(" IPMI(J)");

        printf("\n");
}

static void
log_energy(int fd_cpu, int fd_cpu_dram)
{
        char timebuf[32];
        struct timespec ts_start, ts_next;
        double acc_energy_last_cpu, acc_energy_last_cpu_dram;
        double ipmi_power_last = -1.0;
        double ipmi_energy = 0.0;
        wait_until_aligned_interval();

        print_headers();

        clock_gettime(CLOCK_MONOTONIC, &ts_start);
        ts_next = ts_start;
        acc_energy_last_cpu = read_powercap_energy(fd_cpu);
        if (fd_cpu_dram >= 0)
                acc_energy_last_cpu_dram = read_powercap_energy(fd_cpu_dram);
#ifndef NO_NVML
        read_energy_gpu();
        gpu_powers_last = gpu_powers;
#endif
        while (1)
        {
                uint64_t elapsed_from_start = get_usec_elapsed(&ts_start, &ts_next);
                if (elapsed_from_start >= timeout * 1000000)
                        break;

                ts_next.tv_sec += interval / 1000000;
                ts_next.tv_nsec += (interval % 1000000) * 1000;
                if (ts_next.tv_nsec >= 1000000000)
                {
                        ts_next.tv_sec += 1;
                        ts_next.tv_nsec -= 1000000000;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts_next, NULL);

                double energy_cpu, power_cpu, energy_cpu_dram, power_cpu_dram;
                read_powercap_power_energy(fd_cpu, &power_cpu, &energy_cpu, &acc_energy_last_cpu);
                if (fd_cpu_dram >= 0)
                        read_powercap_power_energy(fd_cpu_dram, &power_cpu_dram, &energy_cpu_dram,
                                                   &acc_energy_last_cpu_dram);
                setup_current_time_str(timebuf);

                printf("%s %.3f", timebuf, power_cpu);
                if (has_energy)
                        printf(" %.3f", energy_cpu);
                if (fd_cpu_dram >= 0)
                {
                        printf(" %.3f", power_cpu_dram);
                        if (has_energy)
                                printf(" %.3f", energy_cpu_dram);
                }
                //read ipmi power
                int power_ipmi = -1;
                if (use_ipmi)
                {
                        power_ipmi = get_ipmi_power_reading(&ipmi_ctx);
                        if (power_ipmi >= 0)
                        {
                                printf(" %d", power_ipmi);
                                if (has_energy)
                                {
                                        ipmi_energy += power_ipmi * (interval / 1000000.0);
                                        printf(" %.3f", ipmi_energy);
                                }
                        }
                }
                setup_current_time_str(timebuf);
#ifndef NO_NVML
                if (use_gpu)
                {
                        double total_power, total_energy;

                        read_energy_gpu();
                        get_total_power_energy_gpu(&total_power, &total_energy);

                        printf(" %.3f", total_power);
                        if (has_energy)
                                printf(" %.3f", total_energy);

                        double* gpu_powers_cur = GET_GPU_POWERS_CUR();
                        if (per_gpu_output)
                        {
                                for (unsigned int i = 0; i < gpu_count; i++)
                                {
                                        printf(" %.3f", gpu_powers_cur[i]);
                                        if (has_energy)
                                                printf(" %.3f", gpu_energies[i]);
                                }
                        }
                        gpu_powers_last = gpu_powers_cur;
                }
#endif
                printf("\n");
                fflush(stdout);
        }
}

static void
parse_args(int argc, char* argv[])
{
        int c;

        while ((c = getopt(argc, argv, "i:t:dDEHhIgG")) != -1)
        {
                switch (c)
                {
                case 'i':
                        interval = strtoul(optarg, NULL, 10) * 1000000;
                        break;
                case 't':
                        timeout = strtoul(optarg, NULL, 10);
                        if (timeout <= 0)
                        {
                                fprintf(stderr, "Invalid timeout value: %s\n", optarg);
                                exit(EXIT_FAILURE);
                        }
                        break;
                case 'd':
                        has_dram = 1;
                        break;
                case 'D':
                        has_mmdd = 1;
                        break;
                case 'E':
                        has_energy = 1;
                        break;
                case 'H':
                        has_headers = 1;
                        break;
                case 'h':
                        usage();
                        exit(0);
                case 'I':
                        use_ipmi = 1;
                        init_ipmi(&ipmi_ctx);
                        break;
                case 'g':
#ifndef NO_NVML
                        use_gpu = 1;
                        break;
                case 'G':
                        use_gpu = 1;
                        per_gpu_output = 1;
                        break;
#endif
                default:
                        usage();
                        exit(EXIT_FAILURE);
                }
        }
}

int
main(int argc, char* argv[])
{
        int fd_cpu, fd_cpu_dram = -1;

        parse_args(argc, argv);

        if (use_ipmi)
        {
                if (init_ipmi(&ipmi_ctx) < 0)
                {
                        fprintf(stderr, "Failed to initialize IPMI. Exiting.\n");
                        exit(EXIT_FAILURE);
                }
        }

#ifndef NO_NVML
        if (use_gpu)
        {
                init_nvml();
        }
#endif

        fd_cpu = open_powercap_pkg0_energy();
        if (has_dram)
                fd_cpu_dram = open_powercap_pkg0_dram_energy();
        log_energy(fd_cpu, fd_cpu_dram);
        close(fd_cpu);
        if (fd_cpu_dram >= 0)
                close(fd_cpu_dram);
        if (use_ipmi)
                close(ipmi_ctx.fd);

#ifndef NO_NVML
        if (use_gpu)
        {
                shutdown_nvml();
        }
#endif

        return 0;
}
