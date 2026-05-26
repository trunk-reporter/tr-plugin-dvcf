# AGENTS.md

This file provides guidance to Codex and other coding agents when working in `tr-plugin-dvcf`.

## What This Project Is

Trunk Recorder plugin that captures raw digital voice codec frames before vocoder synthesis and writes/publishes DVCF files for IMBE-ASR. Digital calls only; analog calls should not produce DVCF.

## Core Files

- `DVCF_SPEC.md` — SymbolStream v2 / DVCF container format.
- `README.md` — build and configuration examples.
- Source files implement Trunk Recorder plugin callbacks such as call start, voice codec data, and call end handling.

## Build Context

This builds as a Trunk Recorder `user_plugins` plugin with local plugins enabled:

```bash
cmake -B build -DUSE_LOCAL_PLUGINS=ON
cmake --build build -j$(nproc)
```

Run these from a Trunk Recorder build tree where this repo is checked out under `user_plugins/mqtt_dvcf`.

## Change Guidance

- Preserve the invariant that DVCF contains `CALL_START`, codec frames, metadata, and `CALL_END` in valid SSSP v2 framing.
- Instrument skipped calls by `audio_type`, `codec_type`, talkgroup, and callback stage when investigating production ratios.
- Keep MQTT and file-writing paths independently configurable.
- Do not block Trunk Recorder callback threads on network or disk work longer than necessary.
