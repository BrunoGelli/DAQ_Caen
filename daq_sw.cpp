#include <CAENDigitizer.h>
#include <TFile.h>
#include <TGraph.h>
#include <TString.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>

#define CHANNEL 0
#define N_EVENTS 1000
#define RECORD_LENGTH 1024  // samples
#define SAMPLING_NS 2.0     // ns per sample (for 500 MS/s DT5730s)


bool stop_requested = false;

void signal_handler(int signum) {
    std::cout << "\nSIGINT received. Cleaning up...\n";
    stop_requested = true;
}

int main() {

    signal(SIGINT, signal_handler);
    int handle;
    int ret;
    char* buffer = nullptr;
    uint32_t bufferSize;
    void* eventPtr = nullptr;
    CAEN_DGTZ_UINT16_EVENT_t* evt = nullptr;
    uint32_t bsize, numEvents;

    // Open digitizer
    ret = CAEN_DGTZ_OpenDigitizer(CAEN_DGTZ_USB, 0, 0, 0, &handle);
    if (ret != CAEN_DGTZ_Success) {
        std::cerr << "Failed to open digitizer\n";
        return -1;
    }

    // Basic setup
    CAEN_DGTZ_SetAcquisitionMode(handle, CAEN_DGTZ_SW_CONTROLLED);
    CAEN_DGTZ_SetChannelEnableMask(handle, 1 << CHANNEL);
    CAEN_DGTZ_SetRecordLength(handle, RECORD_LENGTH);

    // Disable all triggers – use software
    CAEN_DGTZ_SetChannelSelfTrigger(handle, CAEN_DGTZ_TRGMODE_DISABLED, 0xFF);
    CAEN_DGTZ_SetExtTriggerInputMode(handle, CAEN_DGTZ_TRGMODE_DISABLED);

    // Allocate memory
    CAEN_DGTZ_MallocReadoutBuffer(handle, &buffer, &bufferSize);
    CAEN_DGTZ_AllocateEvent(handle, (void**)&evt);

    // Open ROOT file for output
    TFile* fout = new TFile("waveforms.root", "RECREATE");
    fout->mkdir("waveforms");
    fout->cd("waveforms");

    // Start acquisition
    CAEN_DGTZ_SWStartAcquisition(handle);

    int acquired = 0;
    int tried = 0;
    std::cout << "Acquiring " << N_EVENTS << " waveforms and saving to ROOT...\n";

    while (acquired < N_EVENTS && !stop_requested) {
        CAEN_DGTZ_SendSWtrigger(handle);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::cout << "Sent SW trigger number " << tried << std::endl;
        tried++;
        CAEN_DGTZ_ReadData(handle, CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT, buffer, &bsize);

        if (bsize == 0)
            continue; // No data, try next trigger

        CAEN_DGTZ_GetNumEvents(handle, buffer, bsize, &numEvents);


        for (uint32_t i = 0; i < numEvents && acquired < N_EVENTS; ++i) {
            CAEN_DGTZ_EventInfo_t eventInfo;
            char* eventData = nullptr;

            CAEN_DGTZ_GetEventInfo(handle, buffer, bsize, i, &eventInfo, &eventData);
            CAEN_DGTZ_DecodeEvent(handle, eventData, &eventPtr);
            evt = (CAEN_DGTZ_UINT16_EVENT_t*)eventPtr;

            int nsamples = evt->ChSize[CHANNEL];
            if (nsamples > 0) {
                std::vector<double> x(nsamples), y(nsamples);
                for (int s = 0; s < nsamples; ++s) {
                    x[s] = s * SAMPLING_NS;                      // time in ns
                    y[s] = (double)evt->DataChannel[CHANNEL][s]; // ADC value
                }

                TString name = Form("waveform_%03d", acquired);
                TGraph* g = new TGraph(nsamples, x.data(), y.data());
                g->SetName(name);
                g->SetTitle(Form("Waveform %d;Time (ns);ADC", acquired));
                g->Write();

                std::cout << "Saved waveform " << acquired << "\n";
                acquired++;
            }
        }
    }

    // Clean up
    fout->Close();
    delete fout;

    CAEN_DGTZ_SWStopAcquisition(handle);
    CAEN_DGTZ_FreeReadoutBuffer(&buffer);
    CAEN_DGTZ_FreeEvent(handle, (void**)&evt);
    CAEN_DGTZ_ClearData(handle);
    CAEN_DGTZ_Reset(handle);           // 
    CAEN_DGTZ_CloseDigitizer(handle);

    handle = -1;

    std::cout << "All waveforms saved in waveforms.root\n";
    return 0;
}
