/*
 * mqtt_dvcf.cc — Combined SymbolStream v2 .dvcf writer + MQTT publisher.
 *
 * Merges dvcf_writer (incremental file writing) and dvcf_mqtt (MQTT publishing)
 * into a single plugin with independently toggleable features:
 *   - write_enabled (default: true)  — write .dvcf files to disk
 *   - mqtt_enabled  (default: false) — publish to MQTT broker
 *
 * Config:
 *   {
 *     "name": "mqtt_dvcf",
 *     "library": "libmqtt_dvcf",
 *     "write_enabled": true,
 *     "mqtt_enabled": true,
 *     "broker": "tcp://mosquitto:1883",
 *     "topic": "tr-eddie/feeds",
 *     "clientid": "dvcf-handler",
 *     "username": "",
 *     "password": "",
 *     "qos": 0,
 *     "stale_call_timeout_sec": 300
 *   }
 */

#include "../../trunk-recorder/plugin_manager/plugin_api.h"

#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/dll/alias.hpp>
#include <boost/log/trivial.hpp>

#include <mqtt/async_client.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

static const std::string TAG = "[mqtt_dvcf] ";

/* ── SymbolStream Protocol v2 constants ──────────────────────────────── */

static constexpr uint8_t  SSSP_MAGIC_0        = 0x53; // 'S'
static constexpr uint8_t  SSSP_MAGIC_1        = 0x59; // 'Y'
static constexpr uint8_t  SSSP_VERSION         = 0x02;
static constexpr uint8_t  SSSP_MSG_CODEC_FRAME = 0x01;
static constexpr uint8_t  SSSP_MSG_CALL_START  = 0x02;
static constexpr uint8_t  SSSP_MSG_CALL_END    = 0x03;
static constexpr uint8_t  SSSP_MSG_CALL_METADATA = 0x05;

/* ── Packed binary structures ────────────────────────────────────────── */

#pragma pack(push, 1)

struct sssp_header_t {
    uint8_t  magic[2];
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t payload_len;
};

struct sssp_codec_hdr_t {
    uint32_t talkgroup;
    uint32_t src_id;
    uint32_t call_id;
    uint64_t timestamp_us;
    uint8_t  codec_type;
    uint8_t  param_count;
    uint8_t  errs;
    uint8_t  flags;
};

struct sssp_call_start_t {
    uint32_t talkgroup;
    uint64_t frequency_hz;
    uint64_t timestamp_us;
    uint32_t call_id;
    uint8_t  system_name_len;
};

struct sssp_call_end_t {
    uint32_t talkgroup;
    uint32_t call_id;
    uint32_t src_id;
    uint64_t frequency_hz;
    uint32_t duration_ms;
    uint32_t error_count;
    uint8_t  encrypted;
    uint8_t  system_name_len;
};

#pragma pack(pop)

static_assert(sizeof(sssp_header_t)    == 8,  "header must be 8 bytes");
static_assert(sizeof(sssp_codec_hdr_t) == 24, "codec header must be 24 bytes");

/* ── Helpers ─────────────────────────────────────────────────────────── */

static uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

static void fill_header(sssp_header_t &h, uint8_t msg_type, uint32_t payload_len) {
    h.magic[0] = SSSP_MAGIC_0; h.magic[1] = SSSP_MAGIC_1;
    h.version = SSSP_VERSION; h.msg_type = msg_type; h.payload_len = payload_len;
}

static std::string wav_to_dvcf(const std::string &p) {
    auto dot = p.rfind('.');
    return (dot != std::string::npos ? p.substr(0, dot) : p) + ".dvcf";
}

static std::string basename_of(const std::string &p) {
    auto pos = p.rfind('/');
    if (pos == std::string::npos) pos = p.rfind('\\');
    return (pos != std::string::npos) ? p.substr(pos + 1) : p;
}

static std::string bytes_to_base64(const std::vector<uint8_t> &buf) {
    using namespace boost::archive::iterators;
    using It = base64_from_binary<transform_width<std::vector<uint8_t>::const_iterator, 6, 8>>;
    std::string b64(It(buf.begin()), It(buf.end()));
    b64.append((3 - buf.size() % 3) % 3, '=');
    return b64;
}

/* ── Per-call state ──────────────────────────────────────────────────── */

struct CallState {
    long        talkgroup      = 0;
    uint32_t    call_id        = 0;
    uint64_t    freq_hz        = 0;
    uint64_t    start_us       = 0;
    uint64_t    last_active_us = 0;
    std::string short_name;
    uint32_t    frame_count = 0;
    bool        poisoned    = false;

    // Disk writing (write_enabled)
    std::string   tmp_path;
    std::ofstream file;

    // In-memory buffer (mqtt_enabled && !write_enabled)
    std::vector<uint8_t> mem_buf;

    CallState() = default;
    CallState(CallState &&) = default;
    CallState &operator=(CallState &&) = default;
    CallState(const CallState &) = delete;
    CallState &operator=(const CallState &) = delete;

    ~CallState() {
        if (file.is_open()) { file.close(); std::remove(tmp_path.c_str()); }
    }
};

/* ── Plugin class ────────────────────────────────────────────────────── */

class Dvcf_Handler : public Plugin_Api, public virtual mqtt::callback {

    std::mutex mu_;
    std::unordered_map<uintptr_t, CallState> calls_;
    uint32_t next_id_ = 1;

    // Config — features
    bool write_enabled_ = true;
    bool mqtt_enabled_  = false;

    // Config — MQTT
    std::string broker_, topic_, client_id_, username_, password_;
    int qos_ = 0;

    // MQTT runtime
    std::unique_ptr<mqtt::async_client> mqtt_client_;
    std::atomic<bool> mqtt_connected_{false};

    // Disk temp dir
    std::string tmp_dir_;

    // Stale-call reaper
    uint64_t stale_timeout_us_ = 300'000'000;  // default 5 minutes

    /* ── Output helpers (write to file or memory) ────────────────────── */

    void emit(CallState &cs, const void *data, size_t len) {
        if (cs.poisoned) return;
        if (write_enabled_) {
            cs.file.write(static_cast<const char *>(data), len);
            if (!cs.file.good()) {
                BOOST_LOG_TRIVIAL(error) << TAG << "Write failed for call_id="
                    << cs.call_id << " TG=" << cs.talkgroup
                    << ": " << strerror(errno);
                cs.poisoned = true;
            }
        } else {
            auto p = static_cast<const uint8_t *>(data);
            cs.mem_buf.insert(cs.mem_buf.end(), p, p + len);
        }
    }

    void emit_call_start(CallState &cs) {
        uint8_t nlen = static_cast<uint8_t>(std::min(cs.short_name.size(), (size_t)255));
        sssp_header_t hdr; fill_header(hdr, SSSP_MSG_CALL_START, sizeof(sssp_call_start_t) + nlen);
        emit(cs, &hdr, sizeof(hdr));
        sssp_call_start_t m{};
        m.talkgroup = (uint32_t)cs.talkgroup; m.frequency_hz = cs.freq_hz;
        m.timestamp_us = cs.start_us; m.call_id = cs.call_id; m.system_name_len = nlen;
        emit(cs, &m, sizeof(m));
        if (nlen) emit(cs, cs.short_name.data(), nlen);
    }

    void emit_call_metadata(CallState &cs, const Call_Data_t &info) {
        nlohmann::ordered_json meta;

        if (!info.talkgroup_tag.empty())         meta["tg_tag"] = info.talkgroup_tag;
        if (!info.talkgroup_alpha_tag.empty())    meta["tg_alpha_tag"] = info.talkgroup_alpha_tag;
        if (!info.talkgroup_group.empty())        meta["tg_group"] = info.talkgroup_group;
        if (!info.talkgroup_description.empty())  meta["tg_description"] = info.talkgroup_description;

        meta["signal"] = info.signal;
        meta["noise"] = info.noise;
        meta["freq_error"] = info.freq_error;
        meta["spike_count"] = info.spike_count;
        meta["emergency"] = info.emergency;
        meta["priority"] = info.priority;
        meta["phase2_tdma"] = info.phase2_tdma;
        meta["tdma_slot"] = info.tdma_slot;

        if (!info.patched_talkgroups.empty()) {
            nlohmann::ordered_json ptgs = nlohmann::ordered_json::array();
            for (const auto tg : info.patched_talkgroups) ptgs.push_back(tg);
            meta["patched_tgs"] = ptgs;
        }

        if (!info.transmission_source_list.empty()) {
            nlohmann::ordered_json src_list = nlohmann::ordered_json::array();
            for (const auto &src : info.transmission_source_list) {
                src_list.push_back({
                    {"src", src.source}, {"time", src.time}, {"pos", src.position},
                    {"emergency", src.emergency}, {"signal_system", src.signal_system},
                    {"tag", src.tag}
                });
            }
            meta["src_list"] = src_list;
        }

        std::string json_str = meta.dump();
        uint32_t json_len = static_cast<uint32_t>(json_str.size());
        sssp_header_t hdr;
        fill_header(hdr, SSSP_MSG_CALL_METADATA, json_len);
        emit(cs, &hdr, sizeof(hdr));
        emit(cs, json_str.data(), json_len);
    }

    void emit_call_end(CallState &cs, const Call_Data_t &info) {
        uint8_t nlen = static_cast<uint8_t>(std::min(info.short_name.size(), (size_t)255));
        sssp_header_t hdr; fill_header(hdr, SSSP_MSG_CALL_END, sizeof(sssp_call_end_t) + nlen);
        emit(cs, &hdr, sizeof(hdr));
        sssp_call_end_t ce{};
        ce.talkgroup = (uint32_t)info.talkgroup; ce.call_id = cs.call_id;
        ce.src_id = (uint32_t)info.source_num; ce.frequency_hz = (uint64_t)info.freq;
        ce.duration_ms = (uint32_t)(info.length * 1000.0);
        ce.error_count = (uint32_t)info.error_count;
        ce.encrypted = info.encrypted ? 1 : 0; ce.system_name_len = nlen;
        emit(cs, &ce, sizeof(ce));
        if (nlen) emit(cs, info.short_name.data(), nlen);
    }

    /* ── Stream management ───────────────────────────────────────────── */

    CallState *get_or_create(Call *call) {
        auto key = reinterpret_cast<uintptr_t>(call);
        auto it = calls_.find(key);
        if (it != calls_.end()) return &it->second;

        CallState &cs = calls_[key];
        cs.talkgroup      = call->get_talkgroup();
        cs.call_id        = next_id_++;
        cs.freq_hz        = static_cast<uint64_t>(call->get_freq());
        cs.start_us       = now_us();
        cs.last_active_us = cs.start_us;
        cs.short_name     = call->get_short_name();
        return &cs;
    }

    bool ensure_file_open(CallState &cs) {
        if (!write_enabled_ || cs.file.is_open()) return true;
        cs.tmp_path = tmp_dir_ + "/call_" + std::to_string(cs.call_id) + ".dvcf.tmp";
        cs.file.open(cs.tmp_path, std::ios::binary | std::ios::trunc);
        if (!cs.file.is_open()) {
            BOOST_LOG_TRIVIAL(error) << TAG << "Cannot open " << cs.tmp_path
                << ": " << strerror(errno);
            return false;
        }
        return true;
    }

    /* ── Disk finalization ───────────────────────────────────────────── */

    bool finalize_file(const std::string &tmp, const std::string &dst) {
        if (std::rename(tmp.c_str(), dst.c_str()) == 0) return true;
        if (errno == EXDEV) {
            std::ifstream s(tmp, std::ios::binary);
            std::ofstream d(dst, std::ios::binary | std::ios::trunc);
            if (!s || !d) return false;
            d << s.rdbuf();
            if (!d.good()) { d.close(); std::remove(dst.c_str()); return false; }
            d.close(); s.close();
            std::remove(tmp.c_str());
            return true;
        }
        BOOST_LOG_TRIVIAL(error) << TAG << "rename failed: " << tmp << " → " << dst
            << ": " << strerror(errno);
        return false;
    }

    /* ── MQTT helpers ────────────────────────────────────────────────── */

    void mqtt_connect() {
        // SSL verification disabled — typical deployment is LAN-only to a local broker.
        auto ssl = mqtt::ssl_options_builder().verify(false).enable_server_cert_auth(false).finalize();
        auto opts = mqtt::connect_options_builder().clean_session()
            .ssl(ssl).automatic_reconnect(std::chrono::seconds(10), std::chrono::seconds(40)).finalize();
        if (!username_.empty() && !password_.empty()) {
            opts.set_user_name(username_); opts.set_password(password_);
        }
        mqtt_client_ = std::make_unique<mqtt::async_client>(broker_, client_id_);
        mqtt_client_->set_callback(*this);
        try {
            BOOST_LOG_TRIVIAL(info) << TAG << "Connecting to " << broker_;
            mqtt_client_->connect(opts)->wait();
        } catch (const mqtt::exception &e) {
            BOOST_LOG_TRIVIAL(error) << TAG << "MQTT connect failed: " << e.what();
        }
    }

    void mqtt_publish(const std::string &b64, const Call_Data_t &info) {
        if (!mqtt_connected_) return;

        nlohmann::ordered_json src_list = nlohmann::ordered_json::array();
        for (const auto &src : info.transmission_source_list) {
            src_list.push_back({
                {"src", src.source}, {"time", src.time}, {"pos", src.position},
                {"emergency", src.emergency}, {"signal_system", src.signal_system}, {"tag", src.tag}
            });
        }

        nlohmann::ordered_json payload = {
            {"audio_dvcf_base64", b64},
            {"metadata", {
                {"talkgroup", info.talkgroup}, {"talkgroup_tag", info.talkgroup_tag},
                {"talkgroup_alpha_tag", info.talkgroup_alpha_tag},
                {"talkgroup_group", info.talkgroup_group},
                {"freq", info.freq}, {"start_time", info.start_time},
                {"stop_time", info.stop_time},
                {"call_length", info.stop_time - info.start_time},
                {"signal", info.signal}, {"noise", info.noise},
                {"freq_error", info.freq_error}, {"spike_count", info.spike_count},
                {"emergency", info.emergency}, {"priority", info.priority},
                {"phase2_tdma", info.phase2_tdma}, {"tdma_slot", info.tdma_slot},
                {"short_name", info.short_name},
                {"filename", basename_of(info.filename)},
                {"srcList", src_list}
            }}
        };

        if (!info.patched_talkgroups.empty()) {
            nlohmann::ordered_json ptgs = nlohmann::ordered_json::array();
            for (const auto tg : info.patched_talkgroups) ptgs.push_back(tg);
            payload["metadata"]["patched_talkgroups"] = ptgs;
        }

        std::string pub_topic = topic_ + "/dvcf";
        try {
            mqtt_client_->publish(mqtt::message_ptr_builder()
                .topic(pub_topic).payload(payload.dump())
                .qos(qos_).retained(false).finalize());
            BOOST_LOG_TRIVIAL(info) << TAG << "Published TG " << info.talkgroup
                << " (" << b64.size() << " b64 bytes) → " << pub_topic;
        } catch (const mqtt::exception &e) {
            BOOST_LOG_TRIVIAL(error) << TAG << "Publish failed: " << e.what();
        }
    }

    /* ── mqtt::callback ──────────────────────────────────────────────── */

    void connected(const std::string &) override {
        BOOST_LOG_TRIVIAL(info) << TAG << "MQTT connected to " << broker_;
        mqtt_connected_ = true;
    }
    void connection_lost(const std::string &cause) override {
        BOOST_LOG_TRIVIAL(error) << TAG << "MQTT connection lost: " << cause;
        mqtt_connected_ = false;
    }

public:
    Dvcf_Handler() {}
    ~Dvcf_Handler() override = default;

    /* ── Plugin_Api ──────────────────────────────────────────────────── */

    int parse_config(json config_data) override {
        write_enabled_ = config_data.value("write_enabled", true);
        mqtt_enabled_  = config_data.value("mqtt_enabled", false);
        broker_    = config_data.value("broker", "tcp://localhost:1883");
        topic_     = config_data.value("topic", "trunk-recorder");
        client_id_ = config_data.value("clientid", "dvcf-handler");
        username_  = config_data.value("username", "");
        password_  = config_data.value("password", "");
        qos_       = config_data.value("qos", 0);
        stale_timeout_us_ = static_cast<uint64_t>(
            config_data.value("stale_call_timeout_sec", 300)) * 1'000'000ULL;
        if (!topic_.empty() && topic_.back() == '/') topic_.pop_back();

        BOOST_LOG_TRIVIAL(info) << TAG << "write_enabled=" << write_enabled_
            << " mqtt_enabled=" << mqtt_enabled_;
        if (mqtt_enabled_)
            BOOST_LOG_TRIVIAL(info) << TAG << "broker=" << broker_
                << " topic=" << topic_ << "/dvcf qos=" << qos_;
        return 0;
    }

    int start() override {
        if (write_enabled_) {
            tmp_dir_ = "/tmp/mqtt_dvcf_" + std::to_string(getpid());
            if (::mkdir(tmp_dir_.c_str(), 0700) != 0 && errno != EEXIST) {
                BOOST_LOG_TRIVIAL(error) << TAG << "Cannot create " << tmp_dir_
                    << ": " << strerror(errno)
                    << " — file writing DISABLED";
                write_enabled_ = false;
            }
        }
        if (mqtt_enabled_) mqtt_connect();
        BOOST_LOG_TRIVIAL(info) << TAG << "Started";
        return 0;
    }

    int stop() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!calls_.empty())
            BOOST_LOG_TRIVIAL(info) << TAG << "Discarding " << calls_.size() << " incomplete streams";
        calls_.clear();
        if (mqtt_client_ && mqtt_connected_) {
            try { mqtt_client_->disconnect()->wait_for(std::chrono::seconds(5)); }
            catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << TAG << "MQTT disconnect error: " << e.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(warning) << TAG << "MQTT disconnect: unknown exception";
            }
            mqtt_connected_ = false;
        }
        mqtt_client_.reset();
        return 0;
    }

    /* ── poll_one — reap stale calls that never received call_end ───── */

    int poll_one() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (calls_.empty()) return 0;

        uint64_t cutoff = now_us() - stale_timeout_us_;
        for (auto it = calls_.begin(); it != calls_.end(); ) {
            CallState &cs = it->second;
            if (cs.last_active_us >= cutoff) { ++it; continue; }

            if (cs.frame_count > 0 && !cs.poisoned && write_enabled_ && cs.file.is_open()) {
                // Salvage: flush and rename with .stale suffix so data isn't lost
                cs.file.flush();
                cs.file.close();
                std::string dst = cs.tmp_path;
                auto pos = dst.rfind(".tmp");
                if (pos != std::string::npos) dst.replace(pos, 4, ".stale");
                else dst += ".stale";
                if (std::rename(cs.tmp_path.c_str(), dst.c_str()) == 0) {
                    BOOST_LOG_TRIVIAL(warning) << TAG << "Salvaged stale call_id="
                        << cs.call_id << " TG=" << cs.talkgroup
                        << " (" << cs.frame_count << " frames) → " << dst;
                } else {
                    BOOST_LOG_TRIVIAL(warning) << TAG << "Reaping stale call_id="
                        << cs.call_id << " TG=" << cs.talkgroup
                        << " (" << cs.frame_count << " frames, salvage rename failed)";
                }
                cs.tmp_path.clear();  // prevent ~CallState from removing the salvaged file
            } else {
                BOOST_LOG_TRIVIAL(warning) << TAG << "Reaping stale call_id="
                    << cs.call_id << " TG=" << cs.talkgroup
                    << " (" << cs.frame_count << " frames, no call_end)"
                    << (cs.tmp_path.empty() ? "" : " file=" + cs.tmp_path);
                // ~CallState handles cleanup of file + tmp if still open
            }
            it = calls_.erase(it);
        }
        return 0;
    }

    /* ── call_start ──────────────────────────────────────────────────── */

    int call_start(Call *call) override {
        if (!write_enabled_ && !mqtt_enabled_) return 0;
        std::lock_guard<std::mutex> lk(mu_);
        get_or_create(call);
        return 0;
    }

    /* ── voice_codec_data ────────────────────────────────────────────── */

    int voice_codec_data(Call *call, int codec_type, long tgid,
                         uint32_t src_id, const uint32_t *params,
                         int param_count, int errs) override {
        if (!write_enabled_ && !mqtt_enabled_) return 0;

        uint32_t psz = static_cast<uint32_t>(param_count) * sizeof(uint32_t);
        sssp_header_t hdr; fill_header(hdr, SSSP_MSG_CODEC_FRAME, sizeof(sssp_codec_hdr_t) + psz);
        sssp_codec_hdr_t chdr{};
        chdr.talkgroup = (uint32_t)tgid; chdr.src_id = src_id;
        chdr.timestamp_us = now_us(); chdr.codec_type = (uint8_t)codec_type;
        chdr.param_count = (uint8_t)param_count; chdr.errs = (uint8_t)errs;

        std::lock_guard<std::mutex> lk(mu_);
        CallState *cs = get_or_create(call);
        if (!cs) return 0;
        chdr.call_id = cs->call_id;

        if (cs->poisoned) return 0;

        // First frame: open file and write CALL_START header
        if (cs->frame_count == 0) {
            if (!ensure_file_open(*cs)) {
                BOOST_LOG_TRIVIAL(error) << TAG << "Dropping call_id=" << cs->call_id
                    << " TG=" << cs->talkgroup << ": file open failed";
                cs->poisoned = true;
                return 0;
            }
            emit_call_start(*cs);
        }

        emit(*cs, &hdr, sizeof(hdr));
        emit(*cs, &chdr, sizeof(chdr));
        if (param_count > 0) emit(*cs, params, psz);
        cs->last_active_us = chdr.timestamp_us;
        ++cs->frame_count;
        return 0;
    }

    /* ── call_end ────────────────────────────────────────────────────── */

    int call_end(Call_Data_t call_info) override {
        if (!write_enabled_ && !mqtt_enabled_) return 0;
        std::lock_guard<std::mutex> lk(mu_);

        // Find matching stream.  call_end receives Call_Data_t (not Call*),
        // so we match on talkgroup + short_name.  This can mismatch if two
        // concurrent calls share the same TG and system — a trunk-recorder
        // API limitation (no call pointer or call_id in Call_Data_t).
        uintptr_t key = 0;
        for (auto &kv : calls_) {
            if (kv.second.talkgroup == call_info.talkgroup &&
                kv.second.short_name == call_info.short_name) { key = kv.first; break; }
        }
        if (!key) return 0;
        CallState &cs = calls_[key];

        // No usable codec frames — analog calls, poisoned calls, or empty streams.
        if (cs.frame_count == 0 || cs.poisoned) {
            calls_.erase(key);
            return 0;
        }

        // Write CALL_METADATA + CALL_END records
        emit_call_metadata(cs, call_info);
        emit_call_end(cs, call_info);

        std::string b64;

        if (write_enabled_) {
            cs.file.flush();
            if (!cs.file.good()) {
                BOOST_LOG_TRIVIAL(error) << TAG << "Discarding corrupt call_id="
                    << cs.call_id << " TG=" << cs.talkgroup
                    << " (" << cs.frame_count << " frames): write errors on "
                    << cs.tmp_path;
                cs.file.close();
                if (!cs.tmp_path.empty()) std::remove(cs.tmp_path.c_str());
                calls_.erase(key);
                return 0;
            }
            cs.file.close();
            if (call_info.filename.empty()) {
                std::remove(cs.tmp_path.c_str()); cs.tmp_path.clear();
                calls_.erase(key); return 0;
            }
            std::string dvcf = wav_to_dvcf(call_info.filename);
            std::string tmp = cs.tmp_path; cs.tmp_path.clear();
            if (finalize_file(tmp, dvcf)) {
                std::ifstream probe(dvcf, std::ios::binary | std::ios::ate);
                BOOST_LOG_TRIVIAL(info) << TAG << "Wrote " << dvcf
                    << " (" << probe.tellg() << " bytes)";
                // If MQTT also enabled, read the file back for base64
                if (mqtt_enabled_) {
                    probe.seekg(0);
                    std::vector<uint8_t> buf(std::istreambuf_iterator<char>(probe), {});
                    b64 = bytes_to_base64(buf);
                }
            }
        } else if (mqtt_enabled_) {
            // Memory-only path: encode directly from buffer
            b64 = bytes_to_base64(cs.mem_buf);
        }

        if (mqtt_enabled_ && !b64.empty())
            mqtt_publish(b64, call_info);

        calls_.erase(key);
        return 0;
    }

    static boost::shared_ptr<Dvcf_Handler> create() {
        return boost::shared_ptr<Dvcf_Handler>(new Dvcf_Handler());
    }
};

BOOST_DLL_ALIAS(Dvcf_Handler::create, create_plugin)
