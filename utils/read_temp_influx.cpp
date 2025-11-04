#include <CAENDigitizer.h>
#include <curl/curl.h>
#include <unistd.h>       // gethostname
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct Config {
    std::string influx_host = "127.0.0.1";
    int influx_port = 8086;
    std::string influx_db   = "testdb";
    std::string measurement = "DT5730S";
    int interval_sec = 5;
    bool once = false;
    bool verbose = false;
};

static void usage(const char* prog) {
    std::cerr <<
    "Usage: " << prog << " --influx-host <HOST> --influx-port <PORT> --influx-db <DB> --measurement <MEAS>\n"
    "       [--interval <seconds>] [--once] [--verbose]\n\n"
    "Example:\n"
    "  " << prog << " --influx-host 192.168.197.46 --influx-port 8086 \\\n"
    "      --influx-db AmBeHV --measurement DT5730S --interval 5 --verbose\n";
}

static bool parse_args(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* name)->char*{
            if (i+1 >= argc) { std::cerr << "Missing value for " << name << "\n"; exit(2); }
            return argv[++i];
        };

        if      (a == "--influx-host") cfg.influx_host = need_value("--influx-host");
        else if (a == "--influx-port") cfg.influx_port = std::atoi(need_value("--influx-port"));
        else if (a == "--influx-db")   cfg.influx_db   = need_value("--influx-db");
        else if (a == "--measurement") cfg.measurement = need_value("--measurement");
        else if (a == "--interval")    cfg.interval_sec= std::atoi(need_value("--interval"));
        else if (a == "--once")        cfg.once = true;
        else if (a == "--verbose")     cfg.verbose = true;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return false; }
        else { std::cerr << "Unknown arg: " << a << "\n"; usage(argv[0]); return false; }
    }
    return true;
}

static std::string get_hostname() {
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf)-1) == 0) return std::string(buf);
    return "unknown-host";
}

static bool influx_write(const Config& cfg, const std::string& line_protocol) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::ostringstream url;
    url << "http://" << cfg.influx_host << ":" << cfg.influx_port
        << "/write?db=" << cfg.influx_db;

    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, line_protocol.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)line_protocol.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); // short network timeout

    // Optional: quiet down
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK && (http_code == 204 || http_code == 200));
}

// Probe which 'ch' index returns a valid temperature
static int find_temp_channel(int handle, bool verbose) {
    for (int ch = 0; ch < 8; ++ch) {
        uint32_t t = 0;
        auto r = CAEN_DGTZ_ReadTemperature(handle, ch, &t);
        if (r == CAEN_DGTZ_Success && t > 0 && t < 150) {
            if (verbose) std::cerr << "[info] Found temp channel ch=" << ch
                                   << " value=" << t << " C\n";
            return ch;
        }
    }
    // fallback: accept any success, even if 0
    for (int ch = 0; ch < 8; ++ch) {
        uint32_t t = 0;
        auto r = CAEN_DGTZ_ReadTemperature(handle, ch, &t);
        if (r == CAEN_DGTZ_Success) {
            if (verbose) std::cerr << "[info] Found temp channel (loose) ch=" << ch
                                   << " value=" << t << " C\n";
            return ch;
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) return 2;

    // Open digitizer (USB, link 0)
    int handle = -1;
    if (CAEN_DGTZ_OpenDigitizer(CAEN_DGTZ_USB, 0, 0, 0, &handle) != CAEN_DGTZ_Success) {
        std::cerr << "[error] Failed to open digitizer (USB, 0,0,0).\n";
        return 1;
    }

    // Find which temperature channel works
    int temp_ch = find_temp_channel(handle, cfg.verbose);
    if (temp_ch < 0) {
        std::cerr << "[error] Could not find a readable temperature channel.\n";
        CAEN_DGTZ_CloseDigitizer(handle);
        return 1;
    }

    std::string host = get_hostname();
    if (cfg.verbose) {
        std::cerr << "[info] Using measurement='" << cfg.measurement
                  << "', db='" << cfg.influx_db << "', host=" << cfg.influx_host
                  << ":" << cfg.influx_port << "\n";
        std::cerr << "[info] Host tag will be '" << host << "'; temp channel=" << temp_ch << "\n";
    }

    // Init libcurl once
    curl_global_init(CURL_GLOBAL_DEFAULT);

    auto loop_once = [&](bool& ok)->void {
    ok = true;
    std::ostringstream fields;
    fields.setf(std::ios::fixed);
    fields.precision(1);

    // Query up to 8 channels and build Influx line protocol
    for (int ch = 0; ch < 8; ++ch) {
        uint32_t temp_raw = 0;
        CAEN_DGTZ_ErrorCode ret = CAEN_DGTZ_ReadTemperature(handle, ch, &temp_raw);
        if (ret == CAEN_DGTZ_Success && temp_raw < 200) {
            double temp_c = static_cast<double>(temp_raw);
            if (fields.tellp() > 0) fields << ",";
            fields << "temp_ch" << ch << "=" << temp_c;
            if (cfg.verbose)
                std::cerr << "[debug] ch" << ch << " = " << temp_c << " C\n";
        }
    }

    if (fields.tellp() == 0) {
        std::cerr << "[error] No valid temperature channels read.\n";
        ok = false;
        return;
    }

    std::ostringstream lp;
    lp << cfg.measurement
       << ",host=" << host
       << ",device=DT5730S "
       << fields.str();

    std::string line = lp.str();
    if (cfg.verbose)
        std::cerr << "[debug] line-protocol: " << line << "\n";

    bool sent = influx_write(cfg, line);
    if (!sent) {
        std::cerr << "[error] Failed to write to InfluxDB at "
                  << cfg.influx_host << ":" << cfg.influx_port
                  << " (db=" << cfg.influx_db << ")\n";
        ok = false;
        return;
    }

    std::cout << "Temperatures sent: " << fields.str() << std::endl;
};


    if (cfg.once) {
        bool ok = false;
        loop_once(ok);
        curl_global_cleanup();
        CAEN_DGTZ_CloseDigitizer(handle);
        return ok ? 0 : 1;
    }

    // Continuous mode
    while (true) {
        bool ok = false;
        loop_once(ok);
        if (!ok) break;
        std::this_thread::sleep_for(std::chrono::seconds(cfg.interval_sec));
    }

    curl_global_cleanup();
    CAEN_DGTZ_CloseDigitizer(handle);
    return 0;
}
