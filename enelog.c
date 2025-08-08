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

//NVML
#include <nvml.h>

static unsigned long     interval = 1000000; // Default interval in usecs
static unsigned timeout = 120; // Default timeout in seconds

static void
usage(void)
{
        printf(
                "Usage: enelog [-i interval] [-t timeout] [-h]\n"
                "Options:\n"
                "  -i <interval>  Sampling interval in seconds (default: 1 second)\n"
                "  -t <timeout>   Total measurement duration in seconds (default: 120 seconds)\n"
                "  -h             Show this help message and exit\n"
        );
}

static int
open_powercap(void)
{
        int     fd;

        fd = open("/sys/class/powercap/intel-rapl:0/energy_uj", O_RDONLY);
        if (fd < 0) {
                perror("failed to open /sys/class/powercap/intel-rapl:0/energy_uj");
                exit(EXIT_FAILURE);
        }
        return fd;
}

static double
read_energy(int fd)
{
        unsigned long energy_uj;
        char buf[128];

        lseek(fd, 0, SEEK_SET);
        if (read(fd, buf, sizeof(buf)) == -1) {
                perror("failed to read");
                exit(EXIT_FAILURE);
        }
        energy_uj = strtoul(buf, NULL, 10);
        return (double)energy_uj / 1e6;
}

static inline uint64_t
get_usec_elapsed(struct timespec *start, struct timespec *end)
{
        time_t  sec = end->tv_sec - start->tv_sec;
        long    nsec = end->tv_nsec - start->tv_nsec;

        if (nsec < 0) {
                sec -= 1;
                nsec += 1000000000L;
        }
        return (uint64_t)sec * 1000000 + (uint64_t)(nsec / 1000);
}

static void
setup_current_time_str(char *timebuf)
{
        time_t now = time(NULL);
        struct tm       tm_now;
        localtime_r(&now, &tm_now);

        strftime(timebuf, 32, "%m-%d %H:%M:%S", &tm_now);
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

        // 전체 대기 시간 (마이크로초)
        long total_wait = wait_sec * 1000000L + wait_usec;
        if (total_wait > 0)
                usleep(total_wait);
}

/*GPU 전력 측정*/
unsigned int gpu_count = 0;
nvmlDevice_t gpu_handle[16];  //최대 16개

void init_nvml(void)
{
    if (nvmlInit_v2() != NVML_SUCCESS) {
        fprintf(stderr, "NVML init failed\n");
        exit(EXIT_FAILURE);
    }
    nvmlDeviceGetCount_v2(&gpu_count);
    for (unsigned int i = 0; i < gpu_count; i++)
        nvmlDeviceGetHandleByIndex_v2(i, &gpu_handle[i]);
}

void shutdown_nvml(void)
{
    nvmlShutdown();
}

/*GPU 전력 측정 함수*/
static double read_gpu_power(double *p0, double *p1)
{
        unsigned int n=(gpu_count<2)?gpu_count:2;
        double power_w[2]={0.00,0.00};

        //2개의 GPU 전력측정
        for (unsigned int i=0;i<n;i++){
                unsigned int mw=0;
                if(nvmlDeviceGetPowerUsage(gpu_handle[i],&mw)==NVML_SUCCESS){
                        power_w[i]=mw/1000.00;
                }else{
                        power_w[i]=0.00;
                }
        }
        //개별 전력
        *p0=power_w[0];
        *p1=(n>=2)?power_w[1]:0.00;
        //개별 전력과 평균전력 모두 반환
        return (n==0)?0.0:(n==1)?power_w[0]:(power_w[0]+power_w[1])/2.0;
}

static void
log_energy(int fd)
{
        char    timebuf[32];
        struct timespec ts_start, ts_last;
        double  energy_last;

        wait_until_aligned_interval();

        clock_gettime(CLOCK_MONOTONIC, &ts_start);
        ts_last = ts_start;
        energy_last = read_energy(fd);

        while (1) {
                struct timespec ts_cur;

                clock_gettime(CLOCK_MONOTONIC, &ts_cur);
                uint64_t        elapsed_from_start = get_usec_elapsed(&ts_start, &ts_cur);
                if (elapsed_from_start >= timeout * 1000000)
                        break;
                uint64_t        elapsed = get_usec_elapsed(&ts_last, &ts_cur);
                if (elapsed >= interval) { // Convert seconds to microseconds
                        double  energy_cur = read_energy(fd);
                        double  energy = energy_cur - energy_last;
                        double  power = energy / (elapsed / 1000000);

                        //gpu
                        double g0,g1;
                        double gpu_avg= read_gpu_power(&g0, &g1);
                        double gpu_energy=gpu_avg * (elapsed / 1000000);

                        setup_current_time_str(timebuf);
                        printf("%s %.3f %.3f %.3f %.3f %.3f %.3f \n",
                                timebuf, power, energy, g0,g1,gpu_avg, gpu_energy);

                        fflush(stdout);
                        energy_last = energy_cur;
                        ts_last = ts_cur;
                        elapsed -= interval;
                }
                usleep(interval - elapsed);
        }
}

static void
parse_args(int argc, char *argv[])
{
        int     c;

        while ((c = getopt(argc, argv, "i:t:h")) != -1) {
                switch (c) {
                        case 'i':
                                interval = strtoul(optarg, NULL, 10) * 1000000;
                                break;
                        case 't':
                                timeout = strtoul(optarg, NULL, 10);
                                if (timeout <= 0) {
                                        fprintf(stderr, "Invalid timeout value: %s\n", optarg);
                                        exit(EXIT_FAILURE);
                                }
                                break;
                        case 'h':
                                usage();
                                exit(0);
                        default:
                                usage();
                                exit(EXIT_FAILURE);
                }
        }
}


int
main(int argc, char *argv[])
{       
        init_nvml();
        int     fd;

        parse_args(argc, argv);

        fd = open_powercap();

        log_energy(fd);

        close(fd);
        shutdown_nvml();
        return 0;
}
