# CALL_METADATA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a CALL_METADATA message (msg_type=0x05) to .dvcf files so they are self-contained for dataset building.

**Architecture:** New JSON-payload message type within the existing SSSP v2 binary framing. Emitted at call_end time (when Call_Data_t is available), positioned before CALL_END. MQTT path updated for field parity.

**Tech Stack:** C++ (nlohmann::json already available via trunk-recorder), SSSP v2 binary protocol.

**Note:** This project has no test harness — it builds as a shared library within trunk-recorder's cmake system. Verification steps are compile-readiness checks and code inspection.

---

### Task 1: Add CALL_METADATA constant

**Files:**
- Modify: `mqtt_dvcf.cc:55-57` (constants block)

- [ ] **Step 1: Add the new constant after SSSP_MSG_CALL_END**

In `mqtt_dvcf.cc`, after line 57 (`SSSP_MSG_CALL_END`), add:

```cpp
static constexpr uint8_t  SSSP_MSG_CALL_METADATA = 0x05;
```

- [ ] **Step 2: Commit**

```bash
git add mqtt_dvcf.cc
git commit -m "feat: add SSSP_MSG_CALL_METADATA constant (0x05)"
```

---

### Task 2: Add emit_call_metadata method

**Files:**
- Modify: `mqtt_dvcf.cc:210-222` (after emit_call_start, before emit_call_end)

- [ ] **Step 1: Add emit_call_metadata method**

Insert the following method between `emit_call_start()` and `emit_call_end()` in the `Dvcf_Handler` class (after line 208):

```cpp
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
            for (auto tg : info.patched_talkgroups) ptgs.push_back(tg);
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
```

- [ ] **Step 2: Verify the method compiles cleanly by inspection**

Check:
- `nlohmann::ordered_json` is already used in `mqtt_publish()` (line 292) — no new includes needed.
- All `Call_Data_t` fields referenced (`talkgroup_tag`, `talkgroup_alpha_tag`, `talkgroup_group`, `talkgroup_description`, `signal`, `noise`, `freq_error`, `spike_count`, `emergency`, `priority`, `phase2_tdma`, `tdma_slot`, `patched_talkgroups`, `transmission_source_list`) exist in the struct per trunk-recorder's `global_structs.h`.
- `emit()` is the existing method at line 190 that writes to file or memory buffer.

- [ ] **Step 3: Commit**

```bash
git add mqtt_dvcf.cc
git commit -m "feat: add emit_call_metadata() — JSON payload inside SSSP v2 header"
```

---

### Task 3: Wire emit_call_metadata into call_end

**Files:**
- Modify: `mqtt_dvcf.cc:470-471` (call_end method, before emit_call_end)

- [ ] **Step 1: Emit CALL_METADATA before CALL_END**

In the `call_end()` method, change:

```cpp
        // Write CALL_END record
        emit_call_end(cs, call_info);
```

To:

```cpp
        // Write CALL_METADATA + CALL_END records
        emit_call_metadata(cs, call_info);
        emit_call_end(cs, call_info);
```

- [ ] **Step 2: Verify file structure by tracing the call_end flow**

With this change, the message order written to a .dvcf file is:
1. `CALL_START` — emitted during `call_start()`
2. `CODEC_FRAME × N` — emitted during `voice_codec_data()`
3. `CALL_METADATA` — emitted during `call_end()`, before CALL_END
4. `CALL_END` — emitted during `call_end()`, after CALL_METADATA

Both file path (`write_enabled_`) and memory buffer path (`mqtt_enabled_ && !write_enabled_`) go through `emit()`, so both paths get CALL_METADATA.

- [ ] **Step 3: Commit**

```bash
git add mqtt_dvcf.cc
git commit -m "feat: emit CALL_METADATA record before CALL_END in .dvcf files"
```

---

### Task 4: Update mqtt_publish for field parity

**Files:**
- Modify: `mqtt_dvcf.cc:289-311` (mqtt_publish method)

- [ ] **Step 1: Add new metadata fields to the MQTT JSON payload**

Replace the `mqtt_publish` method body (lines 289-323) with:

```cpp
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
            for (auto tg : info.patched_talkgroups) ptgs.push_back(tg);
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
```

- [ ] **Step 2: Commit**

```bash
git add mqtt_dvcf.cc
git commit -m "feat: add signal/noise/emergency/priority/tdma fields to MQTT payload"
```

---

### Task 5: Update DVCF_SPEC.md — message type table and new subsection

**Files:**
- Modify: `DVCF_SPEC.md:246-255` (§5 Message Types table)
- Modify: `DVCF_SPEC.md` (new §3.6 after HEARTBEAT)

- [ ] **Step 1: Update the message type table in §5**

Replace:

```markdown
| 0x05–0xFF    | —              | Reserved; skip via payload_len  |
```

With:

```markdown
| 0x05         | `call_metadata`| Call-level metadata (JSON)      |
| 0x06–0xFF    | —              | Reserved; skip via payload_len  |
```

- [ ] **Step 2: Add §3.6 CALL_METADATA after §3.5 HEARTBEAT**

After the HEARTBEAT section (around line 140), before the `---` separator, add:

````markdown
### 3.6 CALL_METADATA (msg_type = 0x05)

Optional per-call metadata as a JSON payload. Enables .dvcf files to be self-contained
for dataset building without external talkgroup databases.

```
 Offset  Size  Type    Field
 0       N     char[]  UTF-8 JSON object (no null terminator; N = payload_len from header)
```

The JSON object contains call-level metadata from trunk-recorder's `Call_Data_t`. All
fields are optional — omit absent fields rather than setting them to null. Receivers
must ignore unknown keys.

**Fields:**

| Key              | Type     | Description                                    |
|------------------|----------|------------------------------------------------|
| `tg_tag`         | string   | Human-readable talkgroup name                  |
| `tg_alpha_tag`   | string   | Short alphabetic talkgroup label               |
| `tg_group`       | string   | Domain category (e.g., Fire, Police, EMS)      |
| `tg_description` | string   | Full talkgroup description                     |
| `signal`         | float    | Signal level (dBFS)                            |
| `noise`          | float    | Noise floor (dBFS)                             |
| `freq_error`     | int      | Frequency error (Hz)                           |
| `spike_count`    | int      | Spike count                                    |
| `emergency`      | bool     | Emergency call flag                            |
| `priority`       | int      | Call priority level                            |
| `phase2_tdma`    | bool     | P25 Phase 2 TDMA mode                          |
| `tdma_slot`      | int      | TDMA slot (0 or 1)                             |
| `patched_tgs`    | int[]    | Cross-patched talkgroup IDs                    |
| `src_list`       | object[] | Transmission source list (see below)           |

**`src_list` entry fields:**

| Key              | Type   | Description                       |
|------------------|--------|-----------------------------------|
| `src`            | int    | Source radio ID                   |
| `time`           | int    | Transmission start time (epoch)   |
| `pos`            | float  | Position within call (seconds)    |
| `emergency`      | int    | Emergency flag (0 or 1)           |
| `signal_system`  | string | Signal system identifier          |
| `tag`            | string | Unit/radio tag (e.g., "Engine 5") |

**Example:**

```json
{
  "tg_tag": "Fire Dispatch",
  "tg_group": "Fire",
  "signal": -42.5,
  "noise": -110.2,
  "emergency": false,
  "src_list": [
    {"src": 1234567, "time": 1711234567, "pos": 0.0, "emergency": 0, "signal_system": "", "tag": "Engine 5"}
  ]
}
```

In a `.dvcf` file, `CALL_METADATA` is typically emitted after the last `CODEC_FRAME` and
before `CALL_END`. However, receivers must dispatch on `msg_type`, not position — the
message may appear at any point in the stream.
````

- [ ] **Step 3: Commit**

```bash
git add DVCF_SPEC.md
git commit -m "docs: add CALL_METADATA (0x05) to DVCF spec"
```

---

### Task 6: Update DVCF_SPEC.md — file format section

**Files:**
- Modify: `DVCF_SPEC.md:611-615` (§9.1 file structure list)

- [ ] **Step 1: Update the file structure in §9.1**

Replace:

```markdown
1. One `CALL_START` message
2. N × `CODEC_FRAME` messages (one per voice frame, 20ms / 50fps for IMBE)
3. One `CALL_END` message
```

With:

```markdown
1. One `CALL_START` message
2. N × `CODEC_FRAME` messages (one per voice frame, 20ms / 50fps for IMBE)
3. One `CALL_METADATA` message (optional; call-level metadata as JSON, see §3.6)
4. One `CALL_END` message

Receivers must dispatch on `msg_type`, not on message position within the file.
`CALL_METADATA` may appear at any point between `CALL_START` and `CALL_END`.
```

- [ ] **Step 2: Commit**

```bash
git add DVCF_SPEC.md
git commit -m "docs: update file format section with CALL_METADATA in structure"
```

---

### Task 7: Update README

**Files:**
- Modify: `README.md:112-127` (MQTT message format example)
- Modify: `README.md:134-135` (integration diagram — `audio_tap_base64` leftover)

- [ ] **Step 1: Update MQTT JSON example to include new fields**

Replace the existing MQTT JSON example block (lines 112-127) with:

```json
{
  "audio_dvcf_base64": "<base64-encoded .dvcf content>",
  "metadata": {
    "talkgroup": 9170,
    "talkgroup_tag": "Fire Dispatch",
    "talkgroup_alpha_tag": "BUTCO FD",
    "talkgroup_group": "Fire",
    "freq": 855737500,
    "start_time": 1711234567,
    "stop_time": 1711234590,
    "call_length": 23,
    "signal": -42.5,
    "noise": -110.2,
    "freq_error": 12,
    "spike_count": 0,
    "emergency": false,
    "priority": 3,
    "phase2_tdma": false,
    "tdma_slot": 0,
    "short_name": "butco",
    "filename": "9170-1711234567_855737500.wav",
    "srcList": [{"src": 1234567, "time": 1711234567, "pos": 0.0, "emergency": 0, "signal_system": "", "tag": "Engine 5"}]
  }
}
```

- [ ] **Step 2: Fix the integration diagram**

Replace:

```
    → MQTT (audio_tap_base64)
```

With:

```
    → MQTT (audio_dvcf_base64)
```

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: update README with new MQTT metadata fields, fix integration diagram"
```

---

### Task 8: Final review and squash commit

- [ ] **Step 1: Review the full diff**

```bash
git diff HEAD~7 --stat
git diff HEAD~7
```

Verify:
- `mqtt_dvcf.cc`: new constant, new `emit_call_metadata()` method, wired into `call_end()`, updated `mqtt_publish()`
- `DVCF_SPEC.md`: §3.6 added, §5 table updated, §9.1 file structure updated
- `README.md`: MQTT example updated, integration diagram fixed

- [ ] **Step 2: Squash into a single feature commit (optional — user preference)**

```bash
git reset --soft HEAD~7
git commit -m "feat: add CALL_METADATA message type for self-contained .dvcf files

Add msg_type=0x05 (CALL_METADATA) carrying a JSON payload with call-level
metadata from Call_Data_t: talkgroup labels, signal/noise, emergency flag,
priority, TDMA info, patched talkgroups, and full transmission source list.

Emitted before CALL_END so .dvcf files are self-contained for dataset
building. MQTT path updated with matching fields for parity.

DVCF spec and README updated."
```
