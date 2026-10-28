# HydroSense

**A small-AI, offline-first irrigation advisory system for smallholder farmers**
Designed around the "Small AI for Development" challenge theme from the World Bank Group's Global AI and Digital Summit (October 2026): small, targeted AI that works in low-connectivity conditions.

---

## The Problem

Most agricultural AI tools assume smartphones, reliable internet, and app-based interfaces — assumptions that don't hold for many smallholder farmers, especially in rural, low-connectivity regions. Farmers frequently either over-irrigate (wasting water and labor) or under-irrigate (risking crop stress) simply because they lack timely, localized information about their soil conditions.

## The Solution

HydroSense is a low-cost sensor unit that:
1. Continuously senses soil moisture, temperature, humidity, and time since last rainfall
2. Runs a small, purpose-built AI model **directly on the microcontroller** — no cloud dependency for the core decision
3. Alerts the farmer through **SMS**, so no smartphone, app, or data plan is required — only a basic phone with cell signal
4. Also displays status via an onboard **LED indicator** (green/yellow/red) and an audible **buzzer alert**, so the system remains useful even without a phone nearby

This directly reflects the "Small AI" design principle: a small, targeted model solving one real problem well, deployable on constrained hardware, rather than a large general-purpose model assuming ideal conditions.

## System Architecture

```
[Soil sensor] [DHT22 temp/humidity] [Rain-event button]
        \              |                  /
         \             |                 /
          --------- ESP32 -----------
                      |
        Hand-ported neural network (on-device inference)
                      |
              Irrigation decision
              /        |         \
        LED (color) Buzzer    SMS via
                              Africa's Talking API
```

## The Model

- **Data**: No existing public dataset combined soil moisture, weather, and a genuine "irrigation needed" label for this context, so training data was generated using a physics-based simulator — modeling realistic soil-moisture decay (field capacity, wilting point, evapotranspiration driven by temperature/humidity) across three soil types (sandy, loamy, clay), with stochastic rainfall events. 40 simulated plots, 90 days each.
- **Label**: "Will this plot need irrigation within the next 24 hours?" — a genuinely predictive label (based on future moisture trajectory), not a simple threshold reading.
- **Architecture**: 7 inputs → 12 → 8 → 1 (sigmoid), 209 total parameters.
- **Performance**: 97.9–98.1% accuracy on entirely unseen farm plots (train/test split performed by plot, not by row, to avoid data leakage from near-identical consecutive readings).
- **Size**: 28KB as a Keras model → 2.9KB as TFLite (~9.7x reduction). Note: dynamic-range quantization made no further difference at this model size — the network's layers are too small (largest is 96 weights) to cross TFLite's quantization threshold. The size reduction here comes from the lightweight export format itself, not quantization.
- **Latency**: ~0.003ms per prediction (measured on development hardware); comfortably sub-millisecond-to-low-single-digit-millisecond range expected on the ESP32's 240MHz CPU — irrelevant in practice, since the system only needs to check conditions every few minutes.

## On-Device Inference

Two representations of the same trained model exist in this project:
- **`.tflite` file** — the standard export, used as evidence of model size/accuracy/latency.
- **Hand-ported C++ implementation** (running live on the ESP32) — the model's exact trained weights, re-expressed as plain matrix multiplication, ReLU, and sigmoid operations in native C++. This was chosen over the TFLite Micro interpreter because, at 209 parameters, the interpreter's own runtime overhead (typically 10–60KB of flash) would exceed the cost of simply computing the model's math directly. Verified to produce numerically identical predictions to the original Keras model.

*(Optional stretch goal, in progress: a full TFLite Micro on-device implementation, to compare directly against the hand-ported approach.)*

## Hardware / Simulation

Built and tested in Wokwi (ESP32) with PlatformIO:
- Soil moisture: simulated via potentiometer (Wokwi has no native soil-moisture sensor component)
- Temperature/humidity: DHT22
- Rain event: pushbutton (manual trigger, standing in for a rain gauge or weather API in production)
- Status indicator: 3-color LED (green/yellow/red)
- Audible alert: piezo buzzer

## SMS Alerts

Integrated with the Africa's Talking sandbox SMS API. Real HTTP requests, real API responses, delivered to Africa's Talking's simulator. (Twilio was evaluated first but its trial tier now requires an account upgrade before issuing a phone number; Africa's Talking's sandbox required no card and worked immediately for this purpose.)

**Production note**: reaching real farmers at scale would require a funded SMS provider account — a concrete, quantifiable cost of scaling past the demo stage.

## Setup

This project uses the Africa's Talking SMS API and a recipient phone number. Both are kept out of this public repository for privacy and security.

1. Copy `include/secrets.h.example` to `include/secrets.h`
2. Fill in your own values:

```cpp
#define AT_API_KEY "your-africastalking-api-key-here"
#define RECIPIENT_PHONE "+234XXXXXXXXXX"
```

- `AT_API_KEY`: your own Africa's Talking sandbox API key (free account at africastalking.com)
- `RECIPIENT_PHONE`: the number alerts are sent to, in international format (e.g. +234...)

`include/secrets.h` is gitignored and intentionally not included in this repository.

## Known Limitations (stated honestly)

- Soil type is currently a fixed configuration constant, set once per install — no soil-type sensor exists in this build.
- The rain-event trigger is manual (button press); production would use a rain gauge or weather API.
- The buzzer's trigger logic is verified via serial-monitor logging; simulated audio wasn't audible in the VS Code-based Wokwi simulation environment specifically (a known limitation of that tool, not the firmware) — expected to work normally on real ESP32 hardware.
- SMS delivery is currently sandboxed (not yet connected to a production-tier, funded account).

## Author

Peter Onah Apeh — Mechatronics Engineering, Federal University of Technology, Minna, Nigeria