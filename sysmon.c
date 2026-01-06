/**
 * sysmon.c
 * 
 * Low-level System Monitor for Raspberry Pi
 *
 * Reads kernel virtual files (/proc, /sys) to gather telemetry
 * and outputs JSON state to stdout.
 * 
 * Compile: gcc -std=c11 -Wall -Wextra -O2 -o sm sysmon.c
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

/* --- Constants & Configuration --- */
#define THERMAL_ZONE_PATH "/sys/class/thermal/thermal_zone0/temp"
#define PROC_STAT_PATH    "/proc/stat"
#define PROC_MEMINFO_PATH "/proc/meminfo"
#define BUFFER_SIZE       1024
#define JSON_BUFFER_SIZE  2048

/* --- Data Structures --- */

typedef struct {
    uint64_t user;
    uint64_t nice;
    uint64_t system;
    uint64_t idle;
    uint64_t iowait;
    uint64_t irq;
    uint64_t softirq;
    uint64_t steal;
} CpuSnapshot;

typedef struct {
    double temp_c;
    double cpu_usage_percent;
    uint64_t mem_total_kb;
    uint64_t mem_available_kb;
    uint64_t mem_free_kb;
    double uptime_sec;
} SystemState;

/* --- Helper Functions --- */

/**
 * @brief Reads the current system uptime.
 * @return Uptime in seconds (double).
 */
static double get_uptime(void) {
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) return 0.0;

    double uptime = 0.0;
    if (fscanf(fp, "%lf", &uptime) != 1) {
        uptime = 0.0;
    }
    fclose(fp);
    return uptime;
}

/**
 * @brief Reads the CPU temperature from the standard thermal zone.
 * @return Temperature in Celsius.
 */
static double get_cpu_temperature(void) {
    int fd = open(THERMAL_ZONE_PATH, O_RDONLY);
    if (fd < 0) {
        // Fallback or error logging could go here
        return -1.0;
    }

    char buffer[16];
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes_read <= 0) return -1.0;

    buffer[bytes_read] = '\0';
    int millidegrees = atoi(buffer);
    return millidegrees / 1000.0;
}

/**
 * @brief Parses /proc/meminfo for memory stats.
 * @param state Pointer to SystemState to update.
 */
static void get_memory_info(SystemState *state) {
    FILE *fp = fopen(PROC_MEMINFO_PATH, "r");
    if (!fp) return;

    char line[128];
    // Simple parser looking for specific keys
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line, "MemTotal: %lu kB", &state->mem_total_kb);
        } else if (strncmp(line, "MemFree:", 8) == 0) {
            sscanf(line, "MemFree: %lu kB", &state->mem_free_kb);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line, "MemAvailable: %lu kB", &state->mem_available_kb);
        }
    }
    fclose(fp);
}

/**
 * @brief Reads /proc/stat and populates a CpuSnapshot struct.
 * @param snapshot Pointer to the snapshot struct to fill.
 * @return 0 on success, -1 on error.
 */
static int get_cpu_snapshot(CpuSnapshot *snapshot) {
    FILE *fp = fopen(PROC_STAT_PATH, "r");
    if (!fp) return -1;

    char buffer[BUFFER_SIZE];
    if (!fgets(buffer, sizeof(buffer), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    // Parsing the first line: "cpu  ..."
    // Format: cpu  user nice system idle iowait irq softirq steal guest guest_nice
    int ret = sscanf(buffer, "cpu  %lu %lu %lu %lu %lu %lu %lu %lu",
                     &snapshot->user, &snapshot->nice, &snapshot->system,
                     &snapshot->idle, &snapshot->iowait, &snapshot->irq,
                     &snapshot->softirq, &snapshot->steal);

    if (ret < 8) {
        // Handle older kernels or missing steal field
        snapshot->steal = 0;
    }
    return 0;
}

/**
 * @brief Calculates CPU usage percentage between two snapshots.
 * @param prev Previous snapshot.
 * @param curr Current snapshot.
 * @return Usage percentage (0.0 - 100.0).
 */
static double calculate_cpu_usage(const CpuSnapshot *prev, const CpuSnapshot *curr) {
    uint64_t prev_idle = prev->idle + prev->iowait;
    uint64_t curr_idle = curr->idle + curr->iowait;

    uint64_t prev_non_idle = prev->user + prev->nice + prev->system + prev->irq + prev->softirq + prev->steal;
    uint64_t curr_non_idle = curr->user + curr->nice + curr->system + curr->irq + curr->softirq + curr->steal;

    uint64_t prev_total = prev_idle + prev_non_idle;
    uint64_t curr_total = curr_idle + curr_non_idle;

    uint64_t total_diff = curr_total - prev_total;
    uint64_t idle_diff  = curr_idle - prev_idle;

    if (total_diff == 0) return 0.0;

    return (double)(total_diff - idle_diff) / total_diff * 100.0;
}

/**
 * @brief Prints the system state as a compact JSON object.
 */
static void print_json(const SystemState *state) {
    char json_buffer[JSON_BUFFER_SIZE];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    int len = snprintf(json_buffer, JSON_BUFFER_SIZE,
        "{"
        "\"timestamp\":%ld.%09ld,"
        "\"uptime_sec\":%.2f,"
        "\"cpu\":{"
            "\"temp_c\":%.2f,"
            "\"usage_pct\":%.1f"
        "},"
        "\"memory\":{"
            "\"total_kb\":%lu,"
            "\"free_kb\":%lu,"
            "\"available_kb\":%lu,"
            "\"used_pct\":%.1f"
        "}"
        "}\n",
        ts.tv_sec, ts.tv_nsec,
        state->uptime_sec,
        state->temp_c,
        state->cpu_usage_percent,
        state->mem_total_kb,
        state->mem_free_kb,
        state->mem_available_kb,
        (state->mem_total_kb > 0) ? 
            (1.0 - ((double)state->mem_available_kb / state->mem_total_kb)) * 100.0 : 0.0
    );

    if (len > 0) {
        if (write(STDOUT_FILENO, json_buffer, len) < 0) {
            // Error writing to stdout (e.g., broken pipe if piped to another tool)
            exit(EXIT_FAILURE);
        }
    }
}

int main(void) {
    SystemState current_state = {0};
    CpuSnapshot prev_cpu_snap, curr_cpu_snap;

    // Initial snapshot
    if (get_cpu_snapshot(&prev_cpu_snap) != 0) {
        fprintf(stderr, "Failed to read %s\n", PROC_STAT_PATH);
        return EXIT_FAILURE;
    }

    // Unbuffered output for real-time piping
    setvbuf(stdout, NULL, _IONBF, 0);

    while (1) {
        // Sleep for 1 second between updates
        sleep(1);

        // Update CPU Snapshot
        if (get_cpu_snapshot(&curr_cpu_snap) == 0) {
            current_state.cpu_usage_percent = calculate_cpu_usage(&prev_cpu_snap, &curr_cpu_snap);
            // Save current as previous for next iteration
            prev_cpu_snap = curr_cpu_snap;
        } else {
            current_state.cpu_usage_percent = -1.0;
        }

        // Update other metrics
        current_state.temp_c = get_cpu_temperature();
        current_state.uptime_sec = get_uptime();
        get_memory_info(&current_state);

        // Output
        print_json(&current_state);
    }

    return EXIT_SUCCESS;
}
