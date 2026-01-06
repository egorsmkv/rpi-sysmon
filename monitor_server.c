/**
 * monitor_server.c
 * 
 * A lightweight, high-performance HTTP server for Raspberry Pi monitoring.
 * 
 * Parses the latest log entry and renders a visual dashboard.
 * 
 * Compile: gcc -std=c11 -Wall -Wextra -O2 -o monitor_server monitor_server.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <time.h>

// Configuration
#define PORT 8080
#define MONITOR_FILE "monitor.log"
#define READ_CHUNK_SIZE 1024 // Read last 1KB to find last line
#define BACKLOG 10

// Struct to hold parsed data
typedef struct {
    double uptime;
    double cpu_temp;
    double cpu_usage;
    long mem_total;
    long mem_free;
    double mem_used_pct;
} SystemData;

/**
 * Handles error reporting and exits.
 */
void error_die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/**
 * Simple helper to find a value in a JSON-like string.
 * Note: This is a "quick and dirty" parser for the specific known format.
 */
double extract_json_value(const char *json, const char *key) {
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    
    char *pos = strstr(json, search_key);
    if (pos) {
        return strtod(pos + strlen(search_key), NULL);
    }
    return 0.0;
}

/**
 * Formats uptime seconds into HH:MM:SS string
 */
void format_uptime(double seconds, char *buffer, size_t size) {
    int s = (int)seconds;
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sec = s % 60;
    snprintf(buffer, size, "%02d:%02d:%02d", h, m, sec);
}

/**
 * Reads the last line of the monitor file efficiently.
 */
int get_latest_data(SystemData *data) {
    int fd = open(MONITOR_FILE, O_RDONLY);
    if (fd < 0) return -1;

    // Seek to end
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size == 0) {
        close(fd);
        return -1;
    }

    // Determine how much to read (last chunk or full file if small)
    off_t seek_pos = (file_size > READ_CHUNK_SIZE) ? file_size - READ_CHUNK_SIZE : 0;
    lseek(fd, seek_pos, SEEK_SET);

    char buffer[READ_CHUNK_SIZE + 1];
    ssize_t bytes_read = read(fd, buffer, READ_CHUNK_SIZE);
    close(fd);

    if (bytes_read <= 0) return -1;
    buffer[bytes_read] = '\0';

    // Find the last newline to isolate the last complete JSON object
    // We assume the file ends with a newline. We want the line before that.
    char *last_line = NULL;
    char *curr = buffer;
    char *next_line;
    
    // Iterate through lines in the chunk
    while ((next_line = strchr(curr, '\n'))) {
        if (next_line > curr) { // If not an empty line
             // Check if this looks like our JSON start
            if (strstr(curr, "{\"timestamp\"")) {
                last_line = curr;
            }
        }
        curr = next_line + 1;
    }
    
    // If the buffer didn't end with \n or we are at the very end
    if (*curr != '\0' && strstr(curr, "{\"timestamp\"")) {
        last_line = curr;
    }

    if (!last_line) return -1;

    // Parse the found line
    data->uptime = extract_json_value(last_line, "uptime_sec");
    data->cpu_temp = extract_json_value(last_line, "temp_c");
    data->cpu_usage = extract_json_value(last_line, "usage_pct");
    data->mem_total = (long)extract_json_value(last_line, "total_kb");
    data->mem_free = (long)extract_json_value(last_line, "free_kb");
    data->mem_used_pct = extract_json_value(last_line, "used_pct");

    return 0;
}

/**
 * Generates the HTML response
 */
void handle_client(int client_fd) {
    char request_buf[1024];
    // Consume request (ignoring return value as we don't process headers)
    if (read(client_fd, request_buf, sizeof(request_buf)) < 0) {
        // Just continue to close if read fails
    }

    SystemData data = {0};
    int ret = get_latest_data(&data);

    char response[4096];
    char uptime_str[32];
    
    if (ret < 0) {
        snprintf(response, sizeof(response), 
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nNo data available yet.");
        if (write(client_fd, response, strlen(response)) < 0) {
            // Write failed
        }
        close(client_fd);
        return;
    }

    format_uptime(data.uptime, uptime_str, sizeof(uptime_str));

    // Determine colors based on thresholds
    // const char *cpu_color = (data.cpu_temp > 70.0) ? "#ff4444" : (data.cpu_temp > 50.0) ? "#ffbb33" : "#00C851";
    const char *mem_color = (data.mem_used_pct > 80.0) ? "#ff4444" : "#33b5e5";

    snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!DOCTYPE html>"
        "<html><head>"
        "<meta charset=\"UTF-8\">"
        "<meta http-equiv=\"refresh\" content=\"1\">"
        "<title>RPi Dashboard</title>"
        "<style>"
        "body { background-color: #121212; color: #e0e0e0; font-family: 'Segoe UI', sans-serif; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }"
        ".dashboard { background-color: #1e1e1e; padding: 2rem; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); width: 400px; }"
        "h2 { text-align: center; margin-bottom: 1.5rem; color: #ffffff; }"
        ".metric { margin-bottom: 1.5rem; }"
        ".label { display: flex; justify-content: space-between; margin-bottom: 0.5rem; font-weight: bold; }"
        ".bar-bg { background-color: #333; height: 20px; border-radius: 10px; overflow: hidden; }"
        ".bar-fill { height: 100%%; transition: width 0.3s ease; text-align: center; font-size: 12px; line-height: 20px; color: black; font-weight: bold; }"
        ".info-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; text-align: center; margin-top: 1rem; }"
        ".info-box { background: #2c2c2c; padding: 10px; border-radius: 5px; }"
        ".val { font-size: 1.2rem; color: #fff; }"
        ".unit { font-size: 0.8rem; color: #888; }"
        "</style>"
        "</head><body>"
        "<div class=\"dashboard\">"
        "  <h2>Raspberry Pi Monitor</h2>"
        
        "  <div class=\"metric\">"
        "    <div class=\"label\"><span>CPU Usage</span><span>%.1f%%</span></div>"
        "    <div class=\"bar-bg\"><div class=\"bar-fill\" style=\"width: %.1f%%; background-color: %s;\"></div></div>"
        "  </div>"

        "  <div class=\"metric\">"
        "    <div class=\"label\"><span>Memory</span><span>%.1f%%</span></div>"
        "    <div class=\"bar-bg\"><div class=\"bar-fill\" style=\"width: %.1f%%; background-color: %s;\"></div></div>"
        "  </div>"

        "  <div class=\"info-grid\">"
        "    <div class=\"info-box\"><div class=\"val\">%.1f°C</div><div class=\"unit\">Temp</div></div>"
        "    <div class=\"info-box\"><div class=\"val\">%s</div><div class=\"unit\">Uptime</div></div>"
        "    <div class=\"info-box\"><div class=\"val\">%ld MB</div><div class=\"unit\">Free RAM</div></div>"
        "    <div class=\"info-box\"><div class=\"val\">%ld MB</div><div class=\"unit\">Total RAM</div></div>"
        "  </div>"
        "</div>"
        "</body></html>",
        data.cpu_usage, data.cpu_usage, (data.cpu_usage > 80 ? "#ff4444" : "#00C851"),
        data.mem_used_pct, data.mem_used_pct, mem_color,
        data.cpu_temp,
        uptime_str,
        data.mem_free / 1024,
        data.mem_total / 1024
    );

    if (write(client_fd, response, strlen(response)) < 0) {
        // Write failed
    }
    close(client_fd);
}

int main(void) {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int opt = 1;

    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) error_die("socket failed");

    // Force attach to port
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) error_die("setsockopt");

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) error_die("bind failed");
    if (listen(server_fd, BACKLOG) < 0) error_die("listen");

    printf("Visual Monitor Server running on port %d...\n", PORT);

    while (1) {
        if ((client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len)) < 0) {
            perror("accept");
            continue;
        }
        handle_client(client_fd);
    }

    return 0;
}
