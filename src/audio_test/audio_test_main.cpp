// Standalone audio bring-up sketch for the board's NS4168 I2S amplifier.
//
// This is not part of the panel firmware. It builds as its own environment so the
// speaker can be exercised without the display, LVGL, Wi-Fi or the Apex poller in
// the way:
//
//     PIO=~/.platformio/penv/bin/pio
//     $PIO run --target upload --environment audio_test --upload-port /dev/ttyACM0
//     $PIO device monitor --environment audio_test --port /dev/ttyACM0
//
// Flashing this replaces the panel firmware on the board; reflash
// display_4_3_capacitive to get it back.

#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

#include "speech_clip.h"

// From the vendor schematic (docs/JC4827W543/5-IO pin distribution/). The amp's
// CTRL pin is tied high through 1 MOhm, so it is always enabled -- silence has to
// be fed as samples, there is no mute line to pull.
static constexpr int kPinBclk = 42;   // SPECK_BCLK
static constexpr int kPinLrclk = 2;   // SPECK_LRCLK
static constexpr int kPinDin = 41;    // SPECK_DIN

static constexpr i2s_port_t kPort = I2S_NUM_0;
static constexpr int kToneRate = 32000;

// The speaker on P7 is 1 W into 8 ohms and the NS4168 will push more than that,
// so full scale is louder than the cone wants for a sustained tone. Speech has a
// low enough duty cycle not to care; 0.80 leaves the tones around half the
// speaker's rating and still has room on the '+' key.
static float g_volume = 0.80f;

static void i2s_start() {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = kToneRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  // The NS4168 is mono but takes a stereo frame; every sample is written to both
  // channels so it makes no difference which one the amp happens to sum or pick.
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = kPinBclk;
  pins.ws_io_num = kPinLrclk;
  pins.data_out_num = kPinDin;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  ESP_ERROR_CHECK(i2s_driver_install(kPort, &config, 0, nullptr));
  ESP_ERROR_CHECK(i2s_set_pin(kPort, &pins));
  ESP_ERROR_CHECK(i2s_zero_dma_buffer(kPort));
}

static void set_rate(int rate) {
  ESP_ERROR_CHECK(i2s_set_clk(kPort, rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO));
}

// Writes one mono sample to both channels of a stereo frame.
static void write_frames(const int16_t *mono, size_t count) {
  static int16_t frames[256 * 2];
  size_t written = 0;
  while (written < count) {
    size_t chunk = min(count - written, sizeof(frames) / sizeof(frames[0]) / 2);
    for (size_t i = 0; i < chunk; i++) {
      frames[i * 2] = mono[written + i];
      frames[i * 2 + 1] = mono[written + i];
    }
    size_t bytes = 0;
    i2s_write(kPort, frames, chunk * 2 * sizeof(int16_t), &bytes, portMAX_DELAY);
    written += chunk;
  }
}

static void play_silence(int ms, int rate) {
  static int16_t quiet[256] = {0};
  int frames_left = (rate * ms) / 1000;
  while (frames_left > 0) {
    int chunk = min(frames_left, 256);
    write_frames(quiet, chunk);
    frames_left -= chunk;
  }
}

// A tone that starts and ends at zero amplitude, so the cone is never asked to
// step from silence to full swing -- that step is what makes the click.
static void play_tone(float hz, int ms, float amplitude) {
  set_rate(kToneRate);
  const int total = (kToneRate * ms) / 1000;
  const int fade = min(total / 4, kToneRate / 100);  // up to 10 ms each end
  int16_t buffer[256];
  double phase = 0.0;
  const double step = 2.0 * PI * hz / kToneRate;

  for (int done = 0; done < total;) {
    int chunk = min(total - done, 256);
    for (int i = 0; i < chunk; i++) {
      int n = done + i;
      float envelope = 1.0f;
      if (n < fade) envelope = static_cast<float>(n) / fade;
      else if (n > total - fade) envelope = static_cast<float>(total - n) / fade;
      buffer[i] = static_cast<int16_t>(sin(phase) * 32767.0 * amplitude * g_volume * envelope);
      phase += step;
      if (phase > 2.0 * PI) phase -= 2.0 * PI;
    }
    write_frames(buffer, chunk);
    done += chunk;
  }
}

// Logarithmic sweep -- each octave gets the same time, which is how the ear hears
// it, and it makes a cheap cone's resonances and roll-off obvious.
static void play_sweep(float from_hz, float to_hz, int ms, float amplitude) {
  set_rate(kToneRate);
  const int total = (kToneRate * ms) / 1000;
  const int fade = kToneRate / 50;  // 20 ms
  int16_t buffer[256];
  double phase = 0.0;

  for (int done = 0; done < total;) {
    int chunk = min(total - done, 256);
    for (int i = 0; i < chunk; i++) {
      int n = done + i;
      float t = static_cast<float>(n) / total;
      float hz = from_hz * pow(to_hz / from_hz, t);
      float envelope = 1.0f;
      if (n < fade) envelope = static_cast<float>(n) / fade;
      else if (n > total - fade) envelope = static_cast<float>(total - n) / fade;
      buffer[i] = static_cast<int16_t>(sin(phase) * 32767.0 * amplitude * g_volume * envelope);
      phase += 2.0 * PI * hz / kToneRate;
      if (phase > 2.0 * PI) phase -= 2.0 * PI;
    }
    write_frames(buffer, chunk);
    done += chunk;
  }
}

static void play_speech() {
  set_rate(kSpeechSampleRate);
  int16_t buffer[256];
  for (size_t done = 0; done < kSpeechSampleCount;) {
    size_t chunk = min(kSpeechSampleCount - done, sizeof(buffer) / sizeof(buffer[0]));
    for (size_t i = 0; i < chunk; i++) {
      buffer[i] = static_cast<int16_t>(kSpeechSamples[done + i] * g_volume);
    }
    write_frames(buffer, chunk);
    done += chunk;
  }
  play_silence(80, kSpeechSampleRate);
}

static void print_menu() {
  Serial.println();
  Serial.println("NS4168 speaker test -- BCLK 42, LRCLK 2, DIN 41, speaker on P7");
  Serial.printf("volume %.2f\n", g_volume);
  Serial.println("  1  440 Hz tone            4  speech clip");
  Serial.println("  2  1 kHz tone             5  run everything");
  Serial.println("  3  sweep 100 Hz - 12 kHz  + / -  volume");
}

static void run_all() {
  Serial.println("440 Hz");
  play_tone(440, 600, 0.8f);
  play_silence(200, kToneRate);

  Serial.println("1 kHz");
  play_tone(1000, 600, 0.8f);
  play_silence(200, kToneRate);

  Serial.println("sweep 100 Hz - 12 kHz");
  play_sweep(100, 12000, 3000, 0.7f);
  play_silence(300, kToneRate);

  Serial.println("speech");
  play_speech();
}

void setup() {
  Serial.begin(115200);
  delay(400);
  i2s_start();
  print_menu();
  Serial.println();
  Serial.println("playing the full sequence once...");
  run_all();
  Serial.println("done -- press a key to play again");
}

void loop() {
  // The amp has no mute pin, so idle time is spent feeding it zeros rather than
  // letting the DMA buffers run dry and hum.
  play_silence(50, kToneRate);

  while (Serial.available()) {
    char key = Serial.read();
    switch (key) {
      case '1': Serial.println("440 Hz"); play_tone(440, 600, 0.8f); break;
      case '2': Serial.println("1 kHz"); play_tone(1000, 600, 0.8f); break;
      case '3': Serial.println("sweep"); play_sweep(100, 12000, 3000, 0.7f); break;
      case '4': Serial.println("speech"); play_speech(); break;
      case '5': run_all(); break;
      case '+':
        g_volume = min(1.0f, g_volume + 0.1f);
        Serial.printf("volume %.2f\n", g_volume);
        break;
      case '-':
        g_volume = max(0.0f, g_volume - 0.1f);
        Serial.printf("volume %.2f\n", g_volume);
        break;
      case '?': print_menu(); break;
      default: break;
    }
  }
}
