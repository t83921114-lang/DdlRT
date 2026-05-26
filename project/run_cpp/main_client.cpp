#include "client.h"
#include "toolbox.h"
#include <fstream>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include "config.h"
#include <iomanip>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <vector>
#include <numeric>
#include "encoder.h"

namespace {

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    if (n % 2 == 1)
        return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

void print_throughput_stats(const char *label, const std::vector<double> &mib_per_s)
{
    if (mib_per_s.empty())
        return;
    double med = median(mib_per_s);
    auto mm = std::minmax_element(mib_per_s.begin(), mib_per_s.end());
    double sum = std::accumulate(mib_per_s.begin(), mib_per_s.end(), 0.0);
    double mean = sum / static_cast<double>(mib_per_s.size());
    std::cout << "[bench] " << label << " over " << mib_per_s.size()
              << " samples — median: " << med << " MiB/s, mean: " << mean
              << " MiB/s, min: " << *mm.first << " MiB/s, max: " << *mm.second
              << " MiB/s" << std::endl;
}

} // namespace

int main(int argc, char **argv)
{
    char buff[256];
    getcwd(buff, 256);
    std::string cwf = std::string(argv[0]);
    std::string sys_config_path = std::string(buff) + cwf.substr(1, cwf.rfind('/') - 1) + "/../../config/parameterConfiguration.xml";
    std::cout << "Current working directory: " << sys_config_path << std::endl;

    const ECProject::Config *config = ECProject::Config::getInstance(sys_config_path);
    const bool bench_durable_io = config->BenchDurableIO;
    const int bench_read_delay_sec = config->BenchReadDelaySec;

    std::string coordinator_addr = config->CoordinatorIP + ":" + std::to_string(config->CoordinatorPort);
    if (argc >= 2) {
        coordinator_addr = argv[1];
        std::cout << "Using coordinator address (from argv): " << coordinator_addr << std::endl;
    } else {
        const char *env_addr = std::getenv("COORDINATOR_ADDR");
        if (env_addr && env_addr[0] != '\0') {
            coordinator_addr = env_addr;
            std::cout << "Using coordinator address (from COORDINATOR_ADDR): " << coordinator_addr << std::endl;
        }
    }

    std::string client_ip = "10.10.1.1";
    int client_port = 55555;
    ECProject::Client client(client_ip, client_port, coordinator_addr, sys_config_path);
    std::cout << client.sayHelloToCoordinatorByGrpc("Client ID: " + client_ip + ":" + std::to_string(client_port)) << std::endl;

    std::vector<int> parameters = client.get_parameters();
    int k = parameters[0];
    int r = parameters[1];
    int z = parameters[2];
    if (parameters[4] < 0 || parameters[4] > 4)
    {
        std::cout << "Code type error" << std::endl;
        return -1;
    }
    const int block_size_bytes = parameters[3];
    const int n = k + r + z;

    const long long bytes_per_stripe =
        static_cast<long long>(n) * static_cast<long long>(block_size_bytes);
    const int bench_stripe_num = 64;
    const int bench_warmup_stripes = 8;
    const int bench_write_repeats = 5;
    const int bench_read_repeats = 3;

    const long long write_batch_bytes =
        bytes_per_stripe * static_cast<long long>(bench_stripe_num);
    const long long read_batch_bytes =
        static_cast<long long>(bench_stripe_num) * static_cast<long long>(k) *
        static_cast<long long>(block_size_bytes);

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "[bench config] stripes_per_batch=" << bench_stripe_num
              << "  warmup_stripes=" << bench_warmup_stripes
              << "  write_repeats=" << bench_write_repeats
              << "  read_repeats=" << bench_read_repeats
              << "  durable_io=" << (bench_durable_io ? "true" : "false")
              << "  read_delay_sec=" << bench_read_delay_sec << std::endl;
    if (bench_durable_io)
    {
        std::cout << "[bench] durable write: datanode fsync + proxy ack before commit; "
                  << "cold read: posix_fadvise(DONTNEED) on each block at datanode"
                  << std::endl;
    }

    if (bench_warmup_stripes > 0)
    {
        std::cout << "Warm-up write (" << bench_warmup_stripes << " stripes, not timed)..."
                  << std::endl;
        for (int i = 0; i < bench_warmup_stripes; i++)
            client.set();
    }

    std::vector<double> write_mib_per_s;
    write_mib_per_s.reserve(static_cast<size_t>(bench_write_repeats));

    for (int rep = 0; rep < bench_write_repeats; rep++)
    {
        std::cout << "Timed write batch " << (rep + 1) << "/" << bench_write_repeats
                  << " (" << bench_stripe_num << " stripes)..." << std::endl;
        std::chrono::high_resolution_clock::time_point t0 =
            std::chrono::high_resolution_clock::now();
        for (int i = 0; i < bench_stripe_num; i++)
            client.set();
        std::chrono::high_resolution_clock::time_point t1 =
            std::chrono::high_resolution_clock::now();
        double elapsed =
            std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        if (elapsed <= 0.0)
            elapsed = 1e-9;
        double tp = static_cast<double>(write_batch_bytes) / (1024.0 * 1024.0) / elapsed;
        write_mib_per_s.push_back(tp);
        std::cout << "  wall_time: " << elapsed << " s  throughput: " << tp << " MiB/s"
                  << std::endl;
    }
    print_throughput_stats(bench_durable_io ? "durable write throughput" : "write throughput",
                           write_mib_per_s);

    const int last_batch_first_stripe =
        bench_warmup_stripes + (bench_write_repeats - 1) * bench_stripe_num;
    // Coordinator getBlocks maps global id i -> stripe i/k, block i%k (data blocks only).
    // Do not use stripe*n + offset; that breaks for stripe_id > 0 (e.g. read of stripe 264).
    const int start_block_id = last_batch_first_stripe * k;
    const int end_block_id =
        (last_batch_first_stripe + bench_stripe_num) * k - 1;

    std::cout << "[read test] stripes=" << bench_stripe_num
              << "  stripe_id_range=[" << last_batch_first_stripe << ","
              << (last_batch_first_stripe + bench_stripe_num - 1) << "]"
              << "  block_range=[" << start_block_id << "," << end_block_id << "]"
              << "  total_bytes=" << read_batch_bytes << std::endl;

    if (bench_durable_io && bench_read_delay_sec > 0)
    {
        std::cout << "[bench] waiting " << bench_read_delay_sec
                  << " s before cold read (fadvise is primary; delay is best-effort only)"
                  << std::endl;
        sleep(static_cast<unsigned int>(bench_read_delay_sec));
    }

    std::vector<double> read_mib_per_s;
    read_mib_per_s.reserve(static_cast<size_t>(bench_read_repeats));

    for (int rep = 0; rep < bench_read_repeats; rep++)
    {
        std::cout << "Timed read " << (rep + 1) << "/" << bench_read_repeats << "..."
                  << std::endl;
        std::chrono::high_resolution_clock::time_point t0 =
            std::chrono::high_resolution_clock::now();
        client.get_blocks(start_block_id, end_block_id);
        std::chrono::high_resolution_clock::time_point t1 =
            std::chrono::high_resolution_clock::now();
        double elapsed =
            std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        if (elapsed <= 0.0)
            elapsed = 1e-9;
        double tp = static_cast<double>(read_batch_bytes) / (1024.0 * 1024.0) / elapsed;
        read_mib_per_s.push_back(tp);
        std::cout << "  wall_time: " << elapsed << " s  throughput: " << tp << " MiB/s"
                  << std::endl;
    }
    print_throughput_stats(bench_durable_io ? "durable/cold read throughput" : "read throughput",
                           read_mib_per_s);

    return 0;
}
