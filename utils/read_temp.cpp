#include <CAENDigitizer.h>
#include <cstdint>
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    int handle = -1;

    // Open first USB-connected digitizer (adjust if you use optical link)
    if (CAEN_DGTZ_OpenDigitizer(CAEN_DGTZ_USB, 0, 0, 0, &handle) != CAEN_DGTZ_Success) {
        std::cerr << "Failed to open digitizer.\n";
        return 1;
    }

    std::cout << "Connected. Reading ADC temperature every second...\n";

    while (true) {
        uint32_t temp_raw = 0;
        // NOTE: 'ch' argument — use 0 for the primary ADC/board temp on DT57xx-class boards.
        // Some models expose per-channel sensors; if this returns an error, try ch=1 as well.
        CAEN_DGTZ_ErrorCode ret = CAEN_DGTZ_ReadTemperature(handle, 0 /* ch */, &temp_raw);
        if (ret != CAEN_DGTZ_Success) {
            std::cerr << "Error reading temperature (ret=" << ret << ").\n";
            break;
        }

        // Units: CAEN returns an integer in °C for many models. If yours uses 0.1°C units,
        // this will just show an integer-ish value; you can change to temp_raw / 10.0 if needed.
        std::cout << "ADC Temperature (ch=0): " << temp_raw << " °C" << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    CAEN_DGTZ_CloseDigitizer(handle);
    return 0;
}
