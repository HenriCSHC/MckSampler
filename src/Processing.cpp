#include "Processing.hpp"
#include "helper/DspHelper.hpp"
#include "helper/JackHelper.hpp"
#include "helper/WaveHelper.hpp"
#include "SampleExplorer.hpp"

// System
#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>

// Audio
#include <jack/jack.h>
#include <jack/midiport.h>
#include <sndfile.h>
#include <samplerate.h>

namespace fs = std::filesystem;

static int JackProcess(jack_nframes_t nframes, void *arg)
{
    auto proc = (mck::Processing *)arg;
    return proc->ProcessAudioMidi(nframes);
}

mck::Processing::Processing()
    : m_gui(nullptr),
      m_isInitialized(false),
      m_done(false),
      m_isProcessing(false),
      m_config(),
      m_curConfig(0),
      m_newConfig(1),
      m_updateConfig(false),
      m_configFile(),
      m_configPath(""),
      m_client(nullptr),
      m_midiIn(nullptr),
      m_midiOut(nullptr),
      m_audioOutL(nullptr),
      m_audioOutR(nullptr),
      m_bufferSize(0),
      m_transportStep(-1),
      m_transportRate(0),
      m_sampleRate(0),
      m_numVoices(0),
      m_voiceIdx(0),
      m_triggerActive(false),
      m_samplePackPath(""),
      m_sampleExplorer(nullptr),
      m_samplePacks()
{
}

mck::Processing::~Processing()
{
    if (m_isInitialized)
    {
        Close();
    }
}

bool mck::Processing::Init()
{
    if (m_isInitialized)
    {
        std::fprintf(stderr, "Processing is already initialized\n");
        return false;
    }

    // 0 - Prepare DSP structs
    m_numVoices = SAMPLER_VOICES_PER_PAD * SAMPLER_NUM_PADS;
    m_voiceIdx = 0;

    m_samples.resize(SAMPLER_NUM_PADS);
    m_voices.resize(m_numVoices);

    // 1 - Load Configuration
    std::string homeDir = ConfigFile::GetHomeDir();
    std::filesystem::path configPath(homeDir);
    configPath.append(".mck").append("sampler").append("config.json");
    m_configPath = configPath.string();
    sampler::Config config;
    if (m_configFile.ReadFile(m_configPath))
    {
        m_configFile.GetConfig(config);
    }

    // 2 - Init JACK
    if ((m_client = jack_client_open("MckSampler", JackNullOption, NULL)) == 0)
    {
        std::fprintf(stderr, "JACK server not running?\n");
        return false;
    }
    int err = jack_set_process_callback(m_client, JackProcess, this);

    if (err)
    {
        std::fprintf(stderr, "Failed to set JACK callback, error code %d\n", err);
        return false;
    }

    m_midiIn = jack_port_register(m_client, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    m_midiOut = jack_port_register(m_client, "midi_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
    m_audioOutL = jack_port_register(m_client, "audio_out_l", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    m_audioOutR = jack_port_register(m_client, "audio_out_r", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    m_bufferSize = jack_get_buffer_size(m_client);
    m_sampleRate = jack_get_sample_rate(m_client);
    m_transportRate = m_sampleRate;

    // 3A - Scan Sample Packs
    std::filesystem::path samplePackPath(homeDir);
    samplePackPath.append(".local").append("share").append("mck").append("sampler");
    m_samplePackPath = samplePackPath.string();
    m_sampleExplorer = new SampleExplorer();
    if (m_sampleExplorer->Init(m_bufferSize, m_sampleRate, m_samplePackPath) == false)
    {
        std::fprintf(stderr, "Failed to init SampleExplorer!\n");
        return false;
    }
    m_sampleExplorer->RefreshSamples(m_samplePacks);

    // 5 - Start JACK Processing
    err = jack_activate(m_client);
    if (err)
    {
        std::fprintf(stderr, "Unable to activate JACK client, error code %d\n", err);
        return false;
    }

    // 4 - Set Configuration
    SetConfiguration(config, true);

    // 5 - Initialized Transport

    if (m_transport.Init(m_client, m_config[m_curConfig].tempo) == false)
    {
        return false;
    }
    m_transportThread = std::thread(&mck::Processing::TransportThread, this);

    m_isInitialized = true;
    return true;
}

void mck::Processing::Close()
{
    m_done = true;
    if (m_client != nullptr)
    {
        // Save Connections
        if (m_config[m_curConfig].reconnect)
        {
            jack::GetConnections(m_client, m_midiIn, m_config[m_curConfig].midiInConnections);
            jack::GetConnections(m_client, m_midiOut, m_config[m_curConfig].midiOutConnections);
            jack::GetConnections(m_client, m_audioOutL, m_config[m_curConfig].audioLeftConnections);
            jack::GetConnections(m_client, m_audioOutR, m_config[m_curConfig].audioRightConnections);
        }
        jack_client_close(m_client);
    }

    // Save File
    m_configFile.SetConfig(m_config[m_curConfig]);
    m_configFile.WriteFile(m_configPath);

    m_transportCond.notify_all();
    if (m_transportThread.joinable())
    {
        m_transportThread.join();
    }

    m_isInitialized = false;
}

void mck::Processing::ReceiveMessage(mck::Message &msg)
{
    if (msg.section == "pads")
    {
        if (msg.msgType == "trigger")
        {
            try
            {
                mck::TriggerData data = nlohmann::json::parse(msg.data);
                std::printf("Triggering PAD #%d\n", data.index + 1);
                m_triggerQueue.try_enqueue(std::pair<unsigned, double>(data.index, data.strength));
            }
            catch (std::exception &e)
            {
                return;
            }
        }
    }
    else if (msg.section == "transport")
    {
        if (msg.msgType == "command")
        {
            try
            {
                mck::TransportCommand cmd = nlohmann::json::parse(msg.data);
                m_transport.ApplyCommand(cmd);
            }
            catch (std::exception &e)
            {
                return;
            }
        }
    }
    else if (msg.section == "data")
    {
        if (msg.msgType == "get")
        {
            m_gui->SendMessage("data", "full", m_config[m_curConfig]);
        }
        else if (msg.msgType == "patch")
        {
            auto config = m_config[m_curConfig];
            try
            {
                nlohmann::json j = config;
                nlohmann::json jPatch = nlohmann::json::parse(msg.data);
                config = j.patch(jPatch);
            }
            catch (std::exception &e)
            {
                std::fprintf(stderr, "Failed to apply data patch: %s\n", e.what());
                m_gui->SendMessage("data", "full", m_config[m_curConfig]);
                return;
            }
            SetConfiguration(config);
        }
    }
    else if (msg.section == "samples")
    {
        if (msg.msgType == "get")
        {
            m_sampleExplorer->RefreshSamples(m_samplePacks);
            m_gui->SendMessage("samples", "packs", m_samplePacks);
        }
        else if (msg.msgType == "command")
        {
            SampleCommand cmd;
            try
            {
                cmd = nlohmann::json::parse(msg.data);
            }
            catch (std::exception &e)
            {
                std::fprintf(stderr, "Failed to parse sample command: %s\n", e.what());
                return;
            }
            if (cmd.type == "load")
            {
                WaveInfoDetail info = m_sampleExplorer->LoadSample(cmd.packIdx, cmd.sampleIdx);
                m_gui->SendMessage("samples", "info", info);
            }
            else if (cmd.type == "play")
            {
                WaveInfoDetail info = m_sampleExplorer->PlaySample(cmd.packIdx, cmd.sampleIdx);
                m_gui->SendMessage("samples", "info", info);
            }
            else if (cmd.type == "stop")
            {
                m_sampleExplorer->StopSample();
            }
            else if (cmd.type == "assign")
            {
                AssignSample(cmd);
            }
        }
    }
}

void mck::Processing::SetGuiPtr(GuiWindow *gui)
{
    m_gui = gui;
}

int mck::Processing::ProcessAudioMidi(jack_nframes_t nframes)
{
    if (m_isInitialized == false)
    {
        return 0;
    }

    m_isProcessing = true;

    if (m_updateConfig.load())
    {
        m_curConfig = m_newConfig;
        m_updateConfig = false;
    }

    TransportState ts;
    m_transport.Process(m_midiOut, nframes, ts);

    int stepIdx = -1;
    if (ts.state == TS_RUNNING)
    {
        stepIdx = ts.beat * 4;
        stepIdx += (int)std::floor((double)ts.pulse / (double)ts.nPulses * 4.0);
        stepIdx %= 16;
    }

    void *midi_buf = jack_port_get_buffer(m_midiIn, nframes);

    jack_nframes_t midiEventCount = jack_midi_get_event_count(midi_buf);
    jack_midi_event_t midiEvent;

    bool sysMsg = false;
    unsigned char chan = 0;

    for (unsigned i = 0; i < midiEventCount; i++)
    {
        jack_midi_event_get(&midiEvent, midi_buf, i);
        sysMsg = (midiEvent.buffer[0] & 0xf0) == 0xf0;
        chan = (midiEvent.buffer[0] & 0x0f);

        if (sysMsg == false && chan == m_config[m_curConfig].midiChan)
        {
            /*
            if (configMode == CONFIG_PADS)
            {
                if ((midiEvent.buffer[0] & 0xf0) == 0x90)
                {
                    m_config[m_curConfig].pads[configIdx].tone = (midiEvent.buffer[1] & 0x7f);
                    printf("Saved note %X for pad #%d.\n\n", m_config[m_curConfig].pads[configIdx].tone, configIdx + 1);
                    configIdx += 1;
                    if (configIdx >= m_config[m_curConfig].numPads)
                    {
                        printf("Finished configuration, entering play mode...\n");
                        configMode = false;
                        break;
                    }
                    printf("Please play note for pad #%d on MIDI channel %d:\n", configIdx + 1, m_config[m_curConfig].midiChan + 1);
                    break;
                }
            }
            else if (configMode == CONFIG_CTRLS)
            {
                if ((midiEvent.buffer[0] & 0xf0) == 0xb0)
                {

                    if (configIdx > 0 && (midiEvent.buffer[1] & 0x7f) == m_config[m_curConfig].pads[configIdx - 1].ctrl)
                    {
                        continue;
                    }
                    m_config[m_curConfig].pads[configIdx].ctrl = (midiEvent.buffer[1] & 0x7f);
                    printf("Saved control %X for pad #%d.\n\n", m_config[m_curConfig].pads[configIdx].ctrl, configIdx + 1);
                    configIdx += 1;
                    if (configIdx >= m_config[m_curConfig].numPads)
                    {
                        printf("Finished configuration, entering play mode...\n");
                        configMode = CONFIG_NONE;
                        break;
                    }
                    printf("Please turn the controller for pad #%d on MIDI channel %d:\n", configIdx + 1, m_config[m_curConfig].midiChan + 1);
                    break;
                }
            }
            else
            {*/
            if ((midiEvent.buffer[0] & 0xf0) == 0x90)
            {
                for (unsigned j = 0; j < m_config[m_curConfig].numPads; j++)
                {
                    if ((midiEvent.buffer[1] & 0x7f) == m_config[m_curConfig].pads[j].tone && m_config[m_curConfig].pads[j].available)
                    {
                        m_voices[m_voiceIdx].playSample = true;
                        m_voices[m_voiceIdx].padIdx = j;
                        m_voices[m_voiceIdx].startIdx = midiEvent.time;
                        m_voices[m_voiceIdx].bufferIdx = 0;
                        m_voices[m_voiceIdx].bufferLen = m_config[m_curConfig].pads[j].lengthSamps;
                        m_voices[m_voiceIdx].gainL = ((float)(midiEvent.buffer[2] & 0x7f) / 127.0f) * m_config[m_curConfig].pads[j].gainLeftLin;
                        m_voices[m_voiceIdx].gainR = ((float)(midiEvent.buffer[2] & 0x7f) / 127.0f) * m_config[m_curConfig].pads[j].gainRightLin;
                        m_voices[m_voiceIdx].pitch = m_config[m_curConfig].pads[j].pitch;

                        m_voiceIdx = (m_voiceIdx + 1) % m_numVoices;
                    }
                }
            }
            else if ((midiEvent.buffer[0] & 0xf0) == 0xb0)
            {
                for (unsigned j = 0; j < m_config[m_curConfig].numPads; j++)
                {
                    if ((midiEvent.buffer[1] & 0x7f) == m_config[m_curConfig].pads[j].ctrl)
                    {
                        m_config[m_curConfig].pads[j].gain = (float)(midiEvent.buffer[2] & 0x7f) / 127.0f;
                        //m_config[m_curConfig].pads[j].pitch = ((float)(midiEvent.buffer[2] & 0x7f) / 127.0f) * 1.5f + 0.5;
                    }
                }
            }
        }
    }
    //}

    // GUI DRUM TRIGGER
    std::pair<unsigned, double> trigger;
    while (m_triggerQueue.try_dequeue(trigger))
    {
        unsigned idx = trigger.first;
        double strength = trigger.second;

        if (idx < m_config[m_curConfig].numPads)
        {
            if (m_config[m_curConfig].pads[idx].available)
            {
                m_voices[m_voiceIdx].playSample = true;
                m_voices[m_voiceIdx].padIdx = idx;
                m_voices[m_voiceIdx].startIdx = 0;
                m_voices[m_voiceIdx].bufferIdx = 0;
                m_voices[m_voiceIdx].bufferLen = m_config[m_curConfig].pads[idx].lengthSamps;
                m_voices[m_voiceIdx].gainL = m_config[m_curConfig].pads[idx].gainLeftLin * strength;
                m_voices[m_voiceIdx].gainR = m_config[m_curConfig].pads[idx].gainRightLin * strength;
                m_voices[m_voiceIdx].pitch = m_config[m_curConfig].pads[idx].pitch;

                m_voiceIdx = (m_voiceIdx + 1) % m_numVoices;
            }
        }
    }

    // Transport TRIGGER
    if (stepIdx >= 0 && stepIdx != m_transportStep)
    {
        unsigned padIdx = 0;
        unsigned patternIdx = (unsigned)std::floor((double)stepIdx / 16.0);
        for (auto &pad : m_config[m_curConfig].pads)
        {
            if (pad.available == false)
            {
                padIdx += 1;
                continue;
            }
            if (pad.nPatterns == 0)
            {
                padIdx += 1;
                continue;
            }

            unsigned curPatIdx = patternIdx % pad.nPatterns;
            unsigned curStepIdx = stepIdx % pad.patterns[curPatIdx].nSteps;

            if (pad.patterns[curPatIdx].steps[curStepIdx].active)
            {
                double strength = (double)pad.patterns[curPatIdx].steps[curStepIdx].velocity / 127.0;
                m_voices[m_voiceIdx].playSample = true;
                m_voices[m_voiceIdx].padIdx = padIdx;
                m_voices[m_voiceIdx].startIdx = ts.pulseIdx % m_bufferSize;
                m_voices[m_voiceIdx].bufferIdx = 0;
                m_voices[m_voiceIdx].bufferLen = pad.lengthSamps;
                m_voices[m_voiceIdx].gainL = pad.gainLeftLin * strength;
                m_voices[m_voiceIdx].gainR = pad.gainRightLin * strength;
                m_voices[m_voiceIdx].pitch = pad.pitch;

                m_voiceIdx = (m_voiceIdx + 1) % m_numVoices;
            }
            padIdx += 1;
        }
        m_transportStep = stepIdx;
        m_transportState = ts;
        m_transportCond.notify_one();
        m_transportRate += m_bufferSize;
    }
    else if (m_transportRate >= m_sampleRate || ts.state != m_transportState.state)
    {
        m_transportState = ts;
        m_transportCond.notify_one();
        m_transportRate = 0;
    }
    else
    {
        m_transportRate += m_bufferSize;
    }

    /*
    if (resetSample)
    {
        sf_seek(sndFile, 0, SEEK_SET);
        resetSample = false;
    }*/

    // Update Samples
    for (auto &s : m_samples)
    {
        if (s.update)
        {
            s.update = false;
            s.curSample = 1 - s.curSample;
        }
    }

    jack_default_audio_sample_t *out_l = (jack_default_audio_sample_t *)jack_port_get_buffer(m_audioOutL, nframes);
    jack_default_audio_sample_t *out_r = (jack_default_audio_sample_t *)jack_port_get_buffer(m_audioOutR, nframes);

    memset(out_l, 0, nframes * sizeof(jack_default_audio_sample_t));
    memset(out_r, 0, nframes * sizeof(jack_default_audio_sample_t));

    unsigned len = 0;
    for (auto &v : m_voices)
    {
        if (v.playSample == false)
        {
            continue;
        }

        mck::WaveInfo &info = m_samples[v.padIdx].info[m_samples[v.padIdx].curSample];

        if (info.valid == false)
        {
            v.playSample = false;
            continue;
        }

        std::vector<std::vector<float>> &buffer = m_samples[v.padIdx].buffer[m_samples[v.padIdx].curSample];
        /*
        for (unsigned i = 0; i < s->numChans; i++)
        {
            memset(s->pitchBuffer[i], 0, bufferSize * sizeof(float));
        }*/
        len = std::min(m_bufferSize, v.bufferLen - v.bufferIdx) - v.startIdx;

        if (info.numChans > 1)
        {
            // Compensate Mono Panning Law
            float gainL = std::min(1.0f, v.gainL * std::sqrt(2.0f));
            float gainR = std::min(1.0f, v.gainR * std::sqrt(2.0f));
            for (unsigned i = 0; i < len; i++)
            {
                out_l[v.startIdx + i] += buffer[0][v.bufferIdx + i] * gainL;
                out_r[v.startIdx + i] += buffer[1][v.bufferIdx + i] * gainR;
            }
        }
        else
        {
            for (unsigned i = 0; i < len; i++)
            {
                out_l[v.startIdx + i] += buffer[0][v.bufferIdx + i] * v.gainL;
                out_r[v.startIdx + i] += buffer[0][v.bufferIdx + i] * v.gainR;
            }

            /*
            memcpy(s->pitchBuffer[0] + v.startIdx, s->buffer + v.bufferIdx, len);
            s->pitcher->setPitchScale(v.pitch);
            unsigned processed = 0;
            unsigned procOut = 0;
            while (processed < bufferSize)
            {
                unsigned procIn = s->pitcher->getSamplesRequired();
                procIn = std::min(bufferSize - processed, procIn);
                s->pitcher->process(s->pitchBuffer + processed, procIn, false);
                processed += procIn;
                int avail = s->pitcher->available();
                int actual = s->pitcher->retrieve(s->outBuffer + procOut, avail);
                procOut += actual;
            }

            for (unsigned i = 0; i < len; i++)
            {
                out_l[v.startIdx + i] += s->outBuffer[0][i] * v.gain * 0.707f;
                out_r[v.startIdx + i] += s->outBuffer[0][i] * v.gain * 0.707f;
            }
            */
        }
        v.bufferIdx += len;
        v.startIdx = 0;

        if (v.bufferIdx >= v.bufferLen)
        {
            // Stop Sample
            v.playSample = false;
        }
    }

    m_sampleExplorer->ProcessAudio(out_l, out_r, nframes);

    m_isProcessing = false;
    m_processCond.notify_all();
    return 0;
}

void mck::Processing::TransportThread()
{
    std::unique_lock lock(m_transportMutex);
    while (true)
    {
        m_transportCond.wait(lock);

        if (m_done.load())
        {
            return;
        }

        if (m_gui != nullptr)
        {
            m_gui->SendMessage("transport", "realtime", m_transportState);
        }
    }
}

bool mck::Processing::PrepareSamples()
{
    if (m_isInitialized)
    {
        return false;
    }

    // Prepare Sound Files and Voices
    m_numVoices = 4 * m_config[m_curConfig].numPads;
    m_voiceIdx = 0;

    m_samples.resize(m_config[m_curConfig].numPads);
    m_voices.resize(m_numVoices);

    std::vector<std::vector<float>> tmpBuffer;

    for (unsigned i = 0; i < m_config[m_curConfig].numPads; i++)
    {
        fs::path samplePath(m_samplePackPath);
        samplePath.append(m_config[m_curConfig].pads[i].samplePath);
        if (fs::exists(samplePath) == false)
        {
            m_config[m_curConfig].pads[i].available = false;
            continue;
        }
        if (fs::is_regular_file(samplePath) == false)
        {
            m_config[m_curConfig].pads[i].available = false;
            continue;
        }
        char newSample = 1 - m_samples[i].curSample;
        m_samples[i].info[newSample] = helper::ImportWaveFile(samplePath.string(), m_sampleRate, m_samples[i].buffer[newSample]);
        if (m_samples[i].info[newSample].valid == false)
        {
            m_config[m_curConfig].pads[i].available = false;
            continue;
        }
        m_config[m_curConfig].pads[i].available = true;
        m_samples[i].update = true;
        // init pitcher
        //m_samples[i].pitcher = new RubberBand::RubberBandStretcher(sampleRate, m_samples[i].numChans, RubberBand::RubberBandStretcher::OptionProcessRealTime, 1.0, 1.0);
        //m_samples[i].pitcher->setMaxProcessSize(bufferSize);
    }
    return true;
}

bool mck::Processing::AssignSample(SampleCommand cmd)
{
    sampler::Config config = m_config[m_curConfig];
    if (cmd.padIdx >= config.numPads)
    {
        return false;
    }

    config.pads[cmd.padIdx].samplePath = m_sampleExplorer->GetSamplePath(cmd.packIdx, cmd.sampleIdx);
    if (config.pads[cmd.padIdx].samplePath == "")
    {
        return false;
    }
    config.pads[cmd.padIdx].sampleName = m_sampleExplorer->GetSampleName(cmd.packIdx, cmd.sampleIdx);

    SetConfiguration(config);

    return true;
}

void mck::Processing::SetConfiguration(sampler::Config &config, bool connect)
{
    if (config.pads.size() != SAMPLER_NUM_PADS)
    {
        config.pads.resize(SAMPLER_NUM_PADS);
    }
    config.numPads = config.pads.size();
    std::vector<bool> updateSamples;
    updateSamples.resize(config.numPads, false);
    for (unsigned i = 0; i < config.numPads; i++)
    {
        config.pads[i].available = false;

        fs::path samplePath(config.pads[i].samplePath);
        if (samplePath.is_absolute() == false)
        {
            samplePath = fs::path(m_samplePackPath).append(config.pads[i].samplePath);
        }
        if (fs::exists(samplePath))
        {
            config.pads[i].available = true;
        }
        else
        {
            continue;
        }

        bool updateWave = false;
        if (m_config[m_curConfig].numPads < config.numPads)
        {
            updateWave = true;
        }
        else if (config.pads[i].samplePath != m_config[m_curConfig].pads[i].samplePath)
        {
            updateWave = true;
        }
        else if (config.pads[i].available == false)
        {
            updateWave = true;
        }

        if (updateWave)
        {
            char newSample = 1 - m_samples[i].curSample;
            WaveInfo info = helper::ImportWaveFile(samplePath.string(), m_sampleRate, m_samples[i].buffer[newSample]);
            if (info.valid)
            {
                config.pads[i].available = true;
                config.pads[i].lengthMs = info.lengthMs;
                m_samples[i].info[newSample] = info;
                updateSamples[i] = true;
            }
            else
            {
                config.pads[i].available = false;
            }
        } else if (config.pads[i].available) {
            config.pads[i].lengthMs = std::min(config.pads[i].lengthMs, m_samples[i].info[m_samples[i].curSample].lengthMs);
        }

        config.pads[i].gain = std::min(6.0, std::max(-200.0, config.pads[i].gain));
        config.pads[i].pan = std::min(100.0, std::max(-100.0, config.pads[i].pan));
        double gainLin = DbToLin(config.pads[i].gain);
        config.pads[i].gainLeftLin = gainLin * std::sqrt((double)(100 - config.pads[i].pan) / 200.0);
        config.pads[i].gainRightLin = gainLin * std::sqrt((double)(100 + config.pads[i].pan) / 200.0);
        config.pads[i].lengthSamps = (unsigned)std::floor((double)config.pads[i].lengthMs * (double)m_sampleRate / 1000.0);
    }

    if (m_isProcessing.load())
    {
        // Wait
        std::mutex mutex;
        std::unique_lock<std::mutex> lock(mutex);
        m_processCond.wait(lock);
    }

    for (unsigned i = 0; i < config.numPads; i++)
    {
        if (updateSamples[i])
        {
            m_samples[i].update = true;
        }
    }

    m_newConfig = 1 - m_curConfig;
    m_config[m_newConfig] = config;
    m_updateConfig = true;

    if (m_gui != nullptr)
    {
        m_gui->SendMessage("data", "full", config);
    }
    m_configFile.SetConfig(config);
    m_configFile.WriteFile(m_configPath);

    if (connect)
    {
        // Connect inputs and outputs
        if (config.reconnect)
        {
            if (jack::SetConnections(m_client, m_midiIn, config.midiInConnections, true) == false)
            {
                std::printf("Failed to connect port %s\n", jack_port_name(m_midiIn));
            }
            if (jack::SetConnections(m_client, m_midiOut, config.midiOutConnections, true) == false)
            {
                std::printf("Failed to connect port %s\n", jack_port_name(m_midiOut));
            }
            if (jack::SetConnections(m_client, m_audioOutL, config.audioLeftConnections, false) == false)
            {
                std::printf("Failed to connect port %s\n", jack_port_name(m_audioOutL));
            }
            if (jack::SetConnections(m_client, m_audioOutR, config.audioRightConnections, false) == false)
            {
                std::printf("Failed to connect port %s\n", jack_port_name(m_audioOutR));
            }
        }
    }
}