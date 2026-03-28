# tr-plugin-dvcf

Trunk Recorder plugin that captures raw IMBE/AMBE voice codec frames and saves them as `.dvcf` files and/or publishes them over MQTT.

This is the companion plugin to [IMBE-ASR](https://github.com/trunk-reporter/imbe-asr) — it provides the raw codec data that IMBE-ASR uses to transcribe P25 radio calls **without audio reconstruction**.

## Why

Standard ASR pipelines for P25 radio work like this:

```
codec params → reconstruct audio (libimbe) → mel spectrogram → ASR
```

Every step is lossy. IMBE-ASR skips all of it by reading the codec parameters directly. But to do that, you need the pre-vocoder codec frames — which trunk-recorder normally discards after audio synthesis.

This plugin taps trunk-recorder's `voice_codec_data()` callback to capture the raw frames before they're consumed by the vocoder.

## Features

- **File writing** — saves `.dvcf` sidecar files alongside audio recordings (same base name, `.dvcf` extension)
- **MQTT publishing** — publishes codec frames as `audio_tap_base64` in the MQTT call-end message, compatible with [tr-engine](https://github.com/trunk-reporter/tr-engine)
- Both features independently toggleable via config

## File Format

`.dvcf` = Digital Voice Codec Frames. SymbolStream v2 binary format — see [DVCF_SPEC.md](DVCF_SPEC.md) for the full specification.

Files are a concatenation of binary messages (8-byte SSSP headers + codec frame payloads) representing a single call. They can be:
- Played back for offline transcription
- Streamed directly to a TCP consumer
- Uploaded to the [IMBE-ASR server](https://github.com/trunk-reporter/imbe-asr) via HTTP

## Requirements

- Trunk Recorder with `voice_codec_data()` plugin API (merged in [TrunkRecorder/trunk-recorder#1098](https://github.com/TrunkRecorder/trunk-recorder/pull/1098))
- libmosquitto / Paho MQTT C++ (for MQTT publishing)
- Boost (already required by trunk-recorder)

## Building

Builds as a `user_plugins` drop-in — no fork of trunk-recorder required. Requires trunk-recorder with `voice_codec_data()` plugin API support (v5.0+).

```bash
# 1. Clone trunk-recorder
git clone https://github.com/TrunkRecorder/trunk-recorder.git
cd trunk-recorder

# 2. Drop this plugin into user_plugins/
mkdir -p user_plugins
git clone https://github.com/trunk-reporter/tr-plugin-dvcf user_plugins/mqtt_dvcf

# 3. Build with local plugins enabled
cmake -B build -DUSE_LOCAL_PLUGINS=ON
cmake --build build -j$(nproc)

# 4. Install
sudo cmake --install build
```

The compiled `libmqtt_dvcf.so` will be installed to `/usr/local/lib/trunk-recorder/` (or your configured prefix).

### Dependencies

- Paho MQTT C++ library (`libpaho-mqttpp3-dev`)
- Paho MQTT C library (`libpaho-mqtt3as-dev`)
- Boost (already required by trunk-recorder)

```bash
# Ubuntu/Debian
sudo apt-get install libpaho-mqtt3as-dev libpaho-mqttpp3-dev
```

## Configuration

Add to your trunk-recorder `config.json`:

```json
{
  "plugins": [
    {
      "name": "mqtt_dvcf",
      "library": "libmqtt_dvcf",
      "write_enabled": true,
      "mqtt_enabled": true,
      "broker": "tcp://localhost:1883",
      "topic": "tr/feeds",
      "clientid": "dvcf-plugin",
      "username": "",
      "password": "",
      "qos": 0
    }
  ]
}
```

| Option | Default | Description |
|---|---|---|
| `write_enabled` | `true` | Write `.dvcf` sidecar files to disk |
| `mqtt_enabled` | `false` | Publish codec frames over MQTT |
| `broker` | `tcp://localhost:1883` | MQTT broker URL |
| `topic` | `tr/feeds` | MQTT topic (matches tr-engine ingest topic) |
| `clientid` | `dvcf-plugin` | MQTT client ID |
| `username` | `""` | MQTT username (optional) |
| `password` | `""` | MQTT password (optional) |
| `qos` | `0` | MQTT QoS level |

## MQTT Message Format

When `mqtt_enabled: true`, the plugin appends an `audio_tap_base64` field to the standard trunk-recorder call-end MQTT message:

```json
{
  "call": { ... },
  "audio_tap_base64": "<base64-encoded .dvcf content>"
}
```

[tr-engine](https://github.com/trunk-reporter/tr-engine) recognizes this field and routes the call to the IMBE-ASR transcription provider.

## Integration

```
trunk-recorder + mqtt_dvcf plugin
    → MQTT (audio_tap_base64)
    → tr-engine (STT_PROVIDER=imbe)
    → imbe-asr server
    → transcript stored in database
```

For live streaming (lower latency), see [symbolstream](https://github.com/trunk-reporter/symbolstream) which streams frames in real time over TCP/UDP.

## Related Projects

- [IMBE-ASR](https://github.com/trunk-reporter/imbe-asr) — ASR model that reads .dvcf files directly
- [tr-engine](https://github.com/trunk-reporter/tr-engine) — backend that ingests MQTT and calls IMBE-ASR
- [symbolstream](https://github.com/trunk-reporter/symbolstream) — real-time codec frame streaming plugin
