// DT5730S minimal acquisition – self/ext/sw triggering for X730 family
// v28: ROOT output with per-run tag subdirectory + start/end ADC temperature tree
// Build: g++ -O2 -std=c++17 daq_threshold_v28.cpp -o daq_threshold_v28 $(root-config --cflags --libs) -lCAENDigitizer

/*

# 1) SW run (tag 'sw' by default), append to same ROOT file
./daq_threshold_v28 -n 200 -m sw --root run_all.root

# 2) Threshold run on ch0, delta=5 ADC, same ROOT file under tag 'self'
./daq_threshold_v28 -n 200 -m self -c 0 -t 5 --root run_all.root

# 3) Custom tag (e.g., 'dark'), still a self-trigger run
./daq_threshold_v28 -n 100 -m self -t 10 -c 0 --root run_all.root --tag dark

# 4) Also dump text alongside ROOT
./daq_threshold_v28 -n 50 -m self -t 5 -c 0 --root pulses.root --txtdir txt_out

*/


#include <CAENDigitizer.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <vector>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

// ROOT
#include <TFile.h>
#include <TDirectory.h>
#include <TH1I.h>
#include <TTree.h>

static void die(const char* where, CAEN_DGTZ_ErrorCode ec){
    fprintf(stderr,"[ERR] %s failed (code=%d)\n", where, ec);
    std::exit(1);
}
static void ok(const char* where, CAEN_DGTZ_ErrorCode ec){
    if(ec!=CAEN_DGTZ_Success) die(where,ec);
}

static void ensure_dir_exists(const std::string& dir){
    if(dir.empty()) return;
    struct stat st{};
    if(stat(dir.c_str(), &st)==0){
        if(!S_ISDIR(st.st_mode)){
            fprintf(stderr, "[warn] --txtdir exists but is not a directory: %s\n", dir.c_str());
        }
        return;
    }
    if(mkdir(dir.c_str(), 0755)!=0 && errno!=EEXIST){
        fprintf(stderr, "[warn] could not create directory '%s' (errno=%d)\n", dir.c_str(), errno);
    }
}

static uint32_t measure_pedestal(int handle, int ch, uint32_t maxSamp=200){
    char* rbuf=nullptr; uint32_t rsz=0;
    ok("MallocReadoutBuffer", CAEN_DGTZ_MallocReadoutBuffer(handle,&rbuf,&rsz));
    void* evt=nullptr; ok("AllocateEvent", CAEN_DGTZ_AllocateEvent(handle,&evt));

    ok("SetSWTriggerMode", CAEN_DGTZ_SetSWTriggerMode(handle, CAEN_DGTZ_TRGMODE_ACQ_ONLY));
    ok("SWStartAcquisition", CAEN_DGTZ_SWStartAcquisition(handle));
    ok("SendSWtrigger", CAEN_DGTZ_SendSWtrigger(handle));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    uint32_t bsz=0; ok("ReadData", CAEN_DGTZ_ReadData(handle, CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT, rbuf, &bsz));
    ok("SWStopAcquisition", CAEN_DGTZ_SWStopAcquisition(handle));

    uint32_t nev=0; ok("GetNumEvents", CAEN_DGTZ_GetNumEvents(handle, rbuf, bsz, &nev));
    if(nev==0 || bsz==0){
        fprintf(stderr,"[warn] pedestal: no data, using midscale\n");
        CAEN_DGTZ_FreeEvent(handle,&evt);
        CAEN_DGTZ_FreeReadoutBuffer(&rbuf);
        return 0x8000;
    }
    CAEN_DGTZ_EventInfo_t info; char* ep=nullptr;
    ok("GetEventInfo", CAEN_DGTZ_GetEventInfo(handle, rbuf, bsz, 0, &info, &ep));
    ok("DecodeEvent", CAEN_DGTZ_DecodeEvent(handle, ep, &evt));
    auto* e = (CAEN_DGTZ_UINT16_EVENT_t*)evt;

    if(!e || ch<0 || ch>=8 || e->ChSize[ch]==0){
        fprintf(stderr,"[warn] pedestal: empty channel, midscale\n");
        CAEN_DGTZ_FreeEvent(handle,&evt);
        CAEN_DGTZ_FreeReadoutBuffer(&rbuf);
        return 0x8000;
    }
    uint32_t n = std::min<uint32_t>(e->ChSize[ch], maxSamp);
    uint64_t sum=0;
    for(uint32_t i=0;i<n;++i) sum += e->DataChannel[ch][i];
    uint32_t ped = (uint32_t)(sum/(n?n:1));

    CAEN_DGTZ_FreeEvent(handle,&evt);
    CAEN_DGTZ_FreeReadoutBuffer(&rbuf);
    return ped;
}

static void read_temperatures(int handle, std::vector<uint32_t>& temps /*size 8, UINT_MAX on failure*/){
    temps.assign(8, std::numeric_limits<uint32_t>::max());
    for(int ch=0; ch<8; ++ch){
        uint32_t t=0;
        CAEN_DGTZ_ErrorCode ec = CAEN_DGTZ_ReadTemperature(handle, ch, &t);
        if(ec==CAEN_DGTZ_Success) temps[ch]=t;
        // If unsupported, it will stay as UINT_MAX
    }
}

int main(int argc,char**argv){
    int N=10;
    std::string trig="self"; // sw | self | ext
    int link=0;
    int ch=0;
    int recLen=1024;
    int post=50;                  // %
    uint32_t delta=120;           // relative threshold (ADC) below pedestal for negative pulses
    std::string txt = "";         // single text file (append)
    std::string txtdir = "";      // directory of one file per event
    std::string rootOut = "";     // root output file
    std::string tag = "";         // subdirectory in ROOT; defaults to trigger mode

    auto need = [&](const char*o, int& i)->char*{
        if(i+1>=argc){ fprintf(stderr,"missing after %s\n",o); std::exit(2); }
        return argv[++i];
    };

    for(int i=1;i<argc;++i){
        std::string a=argv[i];
        if(a=="-n") N=std::atoi(need("-n",i));
        else if(a=="-m"||a=="--trigger") trig=need("-m",i);
        else if(a=="--link") link=std::atoi(need("--link",i));
        else if(a=="-c") ch=std::atoi(need("-c",i));
        else if(a=="-r") recLen=std::atoi(need("-r",i));
        else if(a=="--post") post=std::atoi(need("--post",i));
        else if(a=="-t") delta=(uint32_t)std::atoi(need("-t",i));
        else if(a=="--txt") txt = need("--txt",i);
        else if(a=="--txtdir") txtdir = need("--txtdir",i);
        else if(a=="--root") rootOut = need("--root",i);
        else if(a=="--tag") tag = need("--tag",i);
        else if(a=="-h"||a=="--help"){
            printf("Usage: %s [-n N] [-m sw|self|ext] [-c ch] [-r recLen] [--post %%] [-t delta]\n"
                   "            [--txt file] [--txtdir dir] [--root file.root] [--tag name]\n", argv[0]);
            return 0;
        }
    }
    if(tag.empty()) tag = trig;

    printf("[info] N=%d, trig=%s, link=%d, ch=%d, recLen=%d, post=%d%%, delta=%u\n",
           N, trig.c_str(), link, ch, recLen, post, delta);
    if(!txt.empty())    printf("[info] txt='%s'\n", txt.c_str());
    if(!txtdir.empty()){ printf("[info] txtdir='%s'\n", txtdir.c_str()); ensure_dir_exists(txtdir); }
    if(!rootOut.empty()) printf("[info] root='%s' tag='%s'\n", rootOut.c_str(), tag.c_str());

    // Open & reset
    int handle=-1;
    ok("OpenDigitizer", CAEN_DGTZ_OpenDigitizer(CAEN_DGTZ_USB, link, 0, 0, &handle));
    ok("Reset", CAEN_DGTZ_Reset(handle));

    CAEN_DGTZ_BoardInfo_t bi{}; ok("GetInfo", CAEN_DGTZ_GetInfo(handle, &bi));
    printf("[board] Model=%s  ROC=%s  AMC=%s  Ch=%u\n", bi.ModelName, bi.ROC_FirmwareRel, bi.AMC_FirmwareRel, bi.Channels);

    // Program basics
    ok("SetAcqMode", CAEN_DGTZ_SetAcquisitionMode(handle, CAEN_DGTZ_SW_CONTROLLED));
    ok("SetChannelEnableMask", CAEN_DGTZ_SetChannelEnableMask(handle, 0xFF)); // enable all
    ok("SetRecordLength", CAEN_DGTZ_SetRecordLength(handle, recLen));
    ok("SetPostTriggerSize", CAEN_DGTZ_SetPostTriggerSize(handle, post));
    ok("SetMaxNumEventsBLT", CAEN_DGTZ_SetMaxNumEventsBLT(handle, 1023));

    // Polarity/edge for negative pulses
    for(int i=0;i<8;++i){
        ok("SetPulsePolarity", CAEN_DGTZ_SetChannelPulsePolarity(handle, i, CAEN_DGTZ_PulsePolarityNegative));
        ok("SetTrigPolarity",  CAEN_DGTZ_SetTriggerPolarity(handle, i, CAEN_DGTZ_TriggerOnFallingEdge));
    }

    // Put baseline high (≈80%)
    for(int i=0;i<8;++i){
        ok("SetChannelDCOffset", CAEN_DGTZ_SetChannelDCOffset(handle, i, 0x3333));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // Trigger selection
    if(trig=="sw"){
        ok("SetChannelSelfTrigger(DIS)", CAEN_DGTZ_SetChannelSelfTrigger(handle, CAEN_DGTZ_TRGMODE_DISABLED, 0xFF));
        ok("SetExt(DIS)",                CAEN_DGTZ_SetExtTriggerInputMode(handle, CAEN_DGTZ_TRGMODE_DISABLED));
        ok("SetSW(ACQ)",                 CAEN_DGTZ_SetSWTriggerMode(handle, CAEN_DGTZ_TRGMODE_ACQ_ONLY));
        printf("[cfg] software trigger mode\n");
    } else if(trig=="ext"){
        ok("SetChannelSelfTrigger(DIS)", CAEN_DGTZ_SetChannelSelfTrigger(handle, CAEN_DGTZ_TRGMODE_DISABLED, 0xFF));
        ok("SetSW(DIS)",                 CAEN_DGTZ_SetSWTriggerMode(handle, CAEN_DGTZ_TRGMODE_DISABLED));
        ok("SetExt(ACQ)",                CAEN_DGTZ_SetExtTriggerInputMode(handle, CAEN_DGTZ_TRGMODE_ACQ_ONLY));
    } else if(trig=="self"){
        ok("SetSW(DIS)",  CAEN_DGTZ_SetSWTriggerMode(handle, CAEN_DGTZ_TRGMODE_DISABLED));
        ok("SetExt(DIS)", CAEN_DGTZ_SetExtTriggerInputMode(handle, CAEN_DGTZ_TRGMODE_DISABLED));
    } else {
        fprintf(stderr,"[ERR] unknown trigger mode\n"); return 2;
    }

    // Pedestal & absolute threshold
    const uint32_t ped = measure_pedestal(handle, ch, 200);
    const uint32_t thr_abs = (trig=="self") ? ((ped > delta) ? ped - delta : 0u) : ped; // use ped for sw/ext readback printing

    // WaveDump-style pair arming (0–1,2–3,...)
    const int pair_base = (ch % 2 == 0) ? ch : (ch-1);
    const uint32_t pair_mask = (1u << pair_base) | (1u << (pair_base+1));

    if(trig=="self"){
        ok("SetThr(pair_base)",   CAEN_DGTZ_SetChannelTriggerThreshold(handle, pair_base,   thr_abs));
        ok("SetThr(pair_base+1)", CAEN_DGTZ_SetChannelTriggerThreshold(handle, pair_base+1, thr_abs));
        ok("SetChannelSelfTrigger(ACQ_ONLY, pair)", CAEN_DGTZ_SetChannelSelfTrigger(handle, CAEN_DGTZ_TRGMODE_ACQ_ONLY, pair_mask));
        uint32_t thr_rd0=0, thr_rd1=0;
        ok("GetThr0", CAEN_DGTZ_GetChannelTriggerThreshold(handle, pair_base, &thr_rd0));
        ok("GetThr1", CAEN_DGTZ_GetChannelTriggerThreshold(handle, pair_base+1, &thr_rd1));
        printf("[auto] ped(ch%d)=%u  thr_abs(set)=%u  rd_pair={%u,%u}  delta=%u  pair_mask=0x%02x\n",
               ch, ped, thr_abs, thr_rd0, thr_rd1, delta, pair_mask);
    } else {
        printf("[auto] ped(ch%d)=%u  (delta=%u; self-trigger not used in this mode)\n", ch, ped, delta);
    }

    // Temperatures at start
    std::vector<uint32_t> tempStart(8), tempEnd(8);
    read_temperatures(handle, tempStart);

    // Prepare ROOT
    TFile* rfile = nullptr;
    TDirectory* dtag = nullptr;
    TTree* runinfo = nullptr;
    TTree* temps = nullptr;

    int    ri_N=N, ri_ch=ch, ri_recLen=recLen, ri_post=post;
    unsigned ri_delta=delta, ri_ped=ped, ri_thr=thr_abs, ri_pairmask=pair_mask;
    std::string ri_trig = trig, ri_tag = tag;

    int t_when=0; // 0=start,1=end
    uint32_t t_temp[8]; // per-channel temps (UINT_MAX if N/A)

    if(!rootOut.empty()){
        rfile = TFile::Open(rootOut.c_str(), "UPDATE");
        if(!rfile || rfile->IsZombie()){
            // try create
            rfile = TFile::Open(rootOut.c_str(), "RECREATE");
        }
        if(rfile && !rfile->IsZombie()){
            // Run info tree (one entry)
            runinfo = (TTree*)rfile->Get("runinfo");
            if(!runinfo){
                runinfo = new TTree("runinfo","acquisition metadata");
                runinfo->Branch("N",         &ri_N,        "N/I");
                runinfo->Branch("ch",        &ri_ch,       "ch/I");
                runinfo->Branch("recLen",    &ri_recLen,   "recLen/I");
                runinfo->Branch("post",      &ri_post,     "post/I");
                runinfo->Branch("delta",     &ri_delta,    "delta/i");
                runinfo->Branch("ped",       &ri_ped,      "ped/i");
                runinfo->Branch("thr_abs",   &ri_thr,      "thr_abs/i");
                runinfo->Branch("pair_mask", &ri_pairmask, "pair_mask/i");
                runinfo->Branch("trig_mode", &ri_trig);
                runinfo->Branch("tag",       &ri_tag);
            }
            runinfo->Fill();

            // Temps tree (two entries per run)
            temps = (TTree*)rfile->Get("temps");
            if(!temps){
                temps = new TTree("temps","ADC temperatures (C)");
                temps->Branch("when",&t_when,"when/I"); // 0=start,1=end
                temps->Branch("temp", t_temp, "temp[8]/i");
            }
            // write start temps
            t_when = 0;
            for(int i=0;i<8;++i) t_temp[i] = tempStart[i];
            temps->Fill();

            // subdirectory for this run’s waveforms
            if(!(dtag = (TDirectory*)rfile->Get(tag.c_str()))){
                dtag = rfile->mkdir(tag.c_str());
            }
        } else {
            fprintf(stderr,"[warn] cannot create ROOT file '%s'\n", rootOut.c_str());
            rfile = nullptr;
        }
    }

    // Acquire
    ok("ClearData", CAEN_DGTZ_ClearData(handle));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ok("SWStartAcquisition", CAEN_DGTZ_SWStartAcquisition(handle));

    char* rbuf=nullptr; uint32_t rsz=0;
    ok("MallocReadoutBuffer", CAEN_DGTZ_MallocReadoutBuffer(handle,&rbuf,&rsz));
    void* evt=nullptr; ok("AllocateEvent", CAEN_DGTZ_AllocateEvent(handle,&evt));

    // Text output (optional single file)
    std::ofstream txt_out;
    if(!txt.empty()){
        txt_out.open(txt, std::ios::app);
        if(!txt_out.is_open()){
            fprintf(stderr,"[warn] could not open --txt='%s' for append\n", txt.c_str());
        }
    }

    auto lastNote = std::chrono::steady_clock::now();
    int got=0;

    while(got<N){
        if(trig=="sw"){
            CAEN_DGTZ_SendSWtrigger(handle);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        uint32_t bsz=0;
        ok("ReadData", CAEN_DGTZ_ReadData(handle, CAEN_DGTZ_SLAVE_TERMINATED_READOUT_MBLT, rbuf, &bsz));
        if(bsz==0){
            auto now=std::chrono::steady_clock::now();
            if(now-lastNote > std::chrono::seconds(5)){
                printf("[stat] no data yet (waiting for triggers)...\n");
                lastNote=now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        uint32_t nev=0; ok("GetNumEvents", CAEN_DGTZ_GetNumEvents(handle, rbuf, bsz, &nev));
        for(uint32_t i=0;i<nev && got<N;++i){
            CAEN_DGTZ_EventInfo_t info; char* ep=nullptr;
            ok("GetEventInfo", CAEN_DGTZ_GetEventInfo(handle, rbuf, bsz, i, &info, &ep));
            ok("DecodeEvent",  CAEN_DGTZ_DecodeEvent(handle, ep, &evt));
            auto* e=(CAEN_DGTZ_UINT16_EVENT_t*)evt;
            uint32_t ns = e? e->ChSize[ch] : 0;
            printf("[evt] #%d  size=%u  chMask=0x%08x  cnt=%u  ttag=%u  ns=%u\n",
                   got, info.EventSize, info.ChannelMask, info.EventCounter, info.TriggerTimeTag, ns);

            // Text output
            if(ns>0 && ( !txt.empty() || !txtdir.empty() )){
                if(!txtdir.empty()){
                    char tmp[512];
                    snprintf(tmp, sizeof(tmp), "%s/waveform_%d.txt", txtdir.c_str(), got);
                    std::ofstream fout(tmp, std::ios::app);
                    if(fout.is_open()){
                        fout << "# Event " << got << "  tag=" << tag << "  trig=" << trig
                             << "  ch=" << ch << "  size=" << ns
                             << "  cnt=" << info.EventCounter << "  ttag=" << info.TriggerTimeTag << "\n";
                        for(uint32_t s=0; s<ns; ++s) fout << e->DataChannel[ch][s] << "\n";
                        fout << "\n";
                    } else {
                        fprintf(stderr,"[warn] cannot write '%s'\n", tmp);
                    }
                } else if(txt_out.is_open()){
                    txt_out << "# Event " << got << "  tag=" << tag << "  trig=" << trig
                            << "  ch=" << ch << "  size=" << ns
                            << "  cnt=" << info.EventCounter << "  ttag=" << info.TriggerTimeTag << "\n";
                    for(uint32_t s=0; s<ns; ++s) txt_out << e->DataChannel[ch][s] << "\n";
                    txt_out << "\n";
                }
            }

            // ROOT output
            if(rfile && dtag && ns>0){
                dtag->cd();
                char hname[128], htitle[256];
                snprintf(hname,  sizeof(hname),  "wave_ev%06d_ch%d", got, ch);
                snprintf(htitle, sizeof(htitle), "Event %d, ch %d;sample;ADC", got, ch);
                TH1I h(hname, htitle, ns, 0.0, double(ns));
                for(uint32_t s=0; s<ns; ++s) h.SetBinContent(int(s)+1, e->DataChannel[ch][s]);
                h.Write();
                rfile->cd(); // back to root dir
            }

            got++;
        }
    }

    if(txt_out.is_open()) txt_out.close();

    ok("SWStopAcquisition", CAEN_DGTZ_SWStopAcquisition(handle));

    // Temperatures at end
    read_temperatures(handle, tempEnd);
    if(rfile && temps){
        t_when = 1;
        for(int i=0;i<8;++i) t_temp[i] = tempEnd[i];
        temps->Fill();
    }

    // Cleanup digitizer
    if(evt)  CAEN_DGTZ_FreeEvent(handle,&evt);
    if(rbuf) CAEN_DGTZ_FreeReadoutBuffer(&rbuf);
    CAEN_DGTZ_CloseDigitizer(handle);

    // Finalize ROOT
    if(rfile){
        // write trees updated above
        if(temps) temps->Write("", TObject::kOverwrite);
        if(runinfo) runinfo->Write("", TObject::kOverwrite);
        rfile->Write();
        rfile->Close();
        delete rfile;
    }

    printf("[ok] Collected %d events. Bye.\n", got);
    return 0;
}
