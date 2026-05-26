# CALL_METADATA Message Type for Self-Contained .dvcf Files

**Date:** 2026-03-28
**Status:** Approved

## Problem

`.dvcf` files currently capture codec frames and basic call lifecycle data (talkgroup, frequency, duration, encryption, system name) but lack the rich metadata needed to build quality datasets. Trunk-recorder's `Call_Data_t` provides signal quality, domain labels, speaker segmentation, and other fields that are dropped during file writing.

Users who batch-upload `.dvcf` files for ASR training, corpus building, or general P25 analysis have no way to:
- Filter by audio quality (signal/noise) before processing
- Stratify by domain (fire/police/EMS) without external talkgroup databases
- Extract speaker diarization ground truth without the transmission source list
- Identify emergency transmissions

The MQTT path carries some of this data, but the file path — which is the primary ingest mechanism for dataset building — carries none of it.

## Design

### New Message Type: CALL_METADATA (msg_type = 0x05)

A new SSSP v2 message type that carries call-level metadata as a JSON payload inside a standard binary header.

#### Wire Format

```
[8 bytes: SSSP v2 header]
  magic[0] = 0x53 ('S')
  magic[1] = 0x59 ('Y')
  version  = 0x02
  msg_type = 0x05
  payload_len = N (uint32, little-endian)

[N bytes: UTF-8 JSON object, no null terminator]
```

#### JSON Schema

```json
{
  "tg_tag": "Fire Dispatch",
  "tg_alpha_tag": "BUTCO FD",
  "tg_group": "Fire",
  "tg_description": "Butler County Fire Dispatch",
  "signal": -42.5,
  "noise": -110.2,
  "freq_error": 12,
  "spike_count": 0,
  "emergency": false,
  "priority": 3,
  "phase2_tdma": false,
  "tdma_slot": 0,
  "patched_tgs": [9170, 9171],
  "src_list": [
    {
      "src": 1234567,
      "time": 1711234567,
      "pos": 0.0,
      "emergency": 0,
      "signal_system": "",
      "tag": "Engine 5"
    }
  ]
}
```

#### Field Reference

| Field | Type | Source (Call_Data_t) | Purpose |
|-------|------|---------------------|---------|
| `tg_tag` | string | `talkgroup_tag` | Human-readable talkgroup name |
| `tg_alpha_tag` | string | `talkgroup_alpha_tag` | Short alphabetic label |
| `tg_group` | string | `talkgroup_group` | Domain category (Fire/Police/EMS) |
| `tg_description` | string | `talkgroup_description` | Full talkgroup description |
| `signal` | float | `signal` | Signal level (dBFS) |
| `noise` | float | `noise` | Noise floor (dBFS) |
| `freq_error` | int | `freq_error` | Frequency error (Hz) |
| `spike_count` | int | `spike_count` | Spike count |
| `emergency` | bool | `emergency` | Emergency flag |
| `priority` | int | `priority` | Call priority level |
| `phase2_tdma` | bool | `phase2_tdma` | P25 Phase 2 TDMA mode |
| `tdma_slot` | int | `tdma_slot` | TDMA slot (0 or 1) |
| `patched_tgs` | int[] | `patched_talkgroups` | Cross-patched talkgroup IDs |
| `src_list` | object[] | `transmission_source_list` | Full speaker segmentation with timing |

#### Field Rules

- All fields are optional. Omit absent fields rather than setting them to null.
- Receivers must ignore unknown keys (forward-compatible).
- Empty arrays (`patched_tgs`, `src_list`) may be omitted.

### File Structure

CALL_METADATA is emitted at `call_end` time (when `Call_Data_t` is available) and positioned just before CALL_END:

```
1. CALL_START       (binary)
2. CODEC_FRAME x N  (binary, one per voice frame at 50fps)
3. CALL_METADATA    (binary header + JSON payload)
4. CALL_END         (binary)
```

Receivers must dispatch on `msg_type`, not on message position within the file. The spec documents a recommended ordering but explicitly states position-independence. This keeps the door open for future live-streaming scenarios where metadata may be available earlier.

### Backward Compatibility

- Existing readers already follow the SSSP v2 rule: "skip unknown msg_type by reading payload_len bytes." CALL_METADATA (0x05) is transparently skipped by any compliant reader.
- Files without CALL_METADATA remain valid. Consumers should handle both cases.
- No changes to existing message types (CALL_START, CODEC_FRAME, CALL_END, HEARTBEAT).

## Implementation Scope

### Plugin (`mqtt_dvcf.cc`)

1. Add `SSSP_MSG_CALL_METADATA = 0x05` constant.
2. Add `emit_call_metadata(CallState &cs, const Call_Data_t &info)` method:
   - Build JSON object from Call_Data_t fields
   - Serialize to string
   - Emit SSSP header (type=0x05, payload_len=json string length)
   - Emit JSON bytes
3. In `call_end()`: call `emit_call_metadata()` before `emit_call_end()`.

### MQTT path parity

Update `mqtt_publish()` to include the new metadata fields in the MQTT JSON (`signal`, `noise`, `emergency`, `tg_group`, `freq_error`, `spike_count`, `priority`, `phase2_tdma`, `tdma_slot`, `patched_tgs`) so both file and MQTT paths carry equivalent data.

### DVCF_SPEC.md

- Add CALL_METADATA to the msg_type table in section 5 (Message Types).
- Add new subsection documenting the message format, JSON schema, and field reference.
- Update section 9 (File Format) to show CALL_METADATA in the file structure.
- Add a note that receivers must not depend on message ordering.

### Downstream Projects

| Project | Impact |
|---------|--------|
| **IMBE-ASR** | Update .dvcf parser to extract CALL_METADATA when present. Gracefully ignore if absent (older files). |
| **tr-engine** | No changes needed — consumes MQTT JSON, which already works. Benefits from additional fields added to MQTT path. |
| **symbolstream** | No changes — live streaming protocol, not file format. |
