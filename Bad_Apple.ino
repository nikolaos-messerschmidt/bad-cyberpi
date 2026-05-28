#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LittleFS.h>
#include <WiFi.h>

//Screen offsets
#define OFFSET_X 3  
#define OFFSET_Y 2  

//Pin Definitions (From official makeblock arduino repo)
#define TFT_MOSI 2
#define TFT_SCLK 4
#define TFT_CS   12
#define I2C_SDA  19
#define I2C_SCL  18
#define AW_ADDR  0x58
#define PIN_AMP  (1 << 3) 
#define PIN_DC   (1 << 4)
#define PIN_RST  (1 << 5)
#define PIN_BL   (1 << 7)  //Die dummen pins von makeblock und deren komisches pinout. 

//global vars
uint16_t fb[128 * 128]; 
uint8_t expander_p1 = PIN_RST; 
int fps = 24;
int frame_ms = 1000 / 24;
int bytes_per_frame = 2048;

//audio vars
uint8_t* audio_buffer = nullptr;
size_t audio_size = 0;
volatile size_t audio_idx = 44; 
volatile bool play_audio = false;
hw_timer_t * audio_timer = NULL;

//lyrics vars
struct LyricWord {
  int ms;
  String text;
};
struct LyricLine {
  int line_ms;
  std::vector<LyricWord> words;
};
std::vector<LyricLine> lyrics;
int current_lyric_line = -1;
int current_lyric_word = -1;

//I2C expander
void writeExpanderPins() {
  Wire.beginTransmission(AW_ADDR);
  Wire.write(0x02); 
  Wire.write(0x00); 
  Wire.write(expander_p1); 
  Wire.endTransmission();
}

void setBit(uint8_t mask, bool val) {
  if (val) expander_p1 |= mask; else expander_p1 &= ~mask;
  writeExpanderPins();
}

//Turn off annoying speaker noise 
void sleepHardware() {
  setBit(PIN_BL, 0);  //backlight off
  setBit(PIN_AMP, 0); //speaker off
  dacWrite(25, 0);    //disable audio
}

//Wake up hardware
void wakeHardware() {
  setBit(PIN_BL, 1);  //Backlight on
  setBit(PIN_AMP, 1); //Speaker on
}

//Buttons (A and B)
bool isButtonAPressed() {
  Wire.beginTransmission(AW_ADDR); Wire.write(0x00); Wire.endTransmission(false);
  Wire.requestFrom((uint16_t)AW_ADDR, (uint8_t)1);
  return !(Wire.read() & (1 << 6)); //Bit 6 is button A
}

bool isButtonBPressed() {
  Wire.beginTransmission(AW_ADDR); Wire.write(0x00); Wire.endTransmission(false);
  Wire.requestFrom((uint16_t)AW_ADDR, (uint8_t)1);
  return !(Wire.read() & (1 << 5)); //Bit 5 is button B
}

//Audio Timer (11025 Hz = 91 us)
void IRAM_ATTR onAudioTimer() {
  if (play_audio && audio_idx < audio_size) {
    uint8_t s = audio_buffer[audio_idx++];
    //Loundness of sound (You can change the 10 to a lower number to increase the volume)
    uint8_t s_quiet = 128 + ((int8_t)(s - 128) / 10); 
    dacWrite(25, s_quiet);
  }
}

//SPI Display control
void writeCommand(uint8_t cmd) {
  digitalWrite(TFT_CS, LOW); setBit(PIN_DC, 0); SPI.transfer(cmd); digitalWrite(TFT_CS, HIGH);
}
void writeData(const uint8_t* data, size_t len) {
  digitalWrite(TFT_CS, LOW); setBit(PIN_DC, 1); SPI.writeBytes((uint8_t*)data, len); digitalWrite(TFT_CS, HIGH);
}
void writeCommandData(uint8_t cmd, const uint8_t* data, size_t len) {
  writeCommand(cmd); if (len > 0) writeData(data, len);
}

void initSystem() {
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH);
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);
  SPI.setFrequency(30000000); 
  
  Wire.beginTransmission(AW_ADDR); Wire.write(0x7F); Wire.write(0x00); Wire.endTransmission(); delay(10);
  Wire.beginTransmission(AW_ADDR); Wire.write(0x04); Wire.write(0xFF); Wire.endTransmission();
  Wire.beginTransmission(AW_ADDR); Wire.write(0x05); Wire.write(0xFF); Wire.endTransmission();
  Wire.beginTransmission(AW_ADDR); Wire.write(0x12); Wire.write(0xFF); Wire.endTransmission();
  Wire.beginTransmission(AW_ADDR); Wire.write(0x13); Wire.write(0xFF); Wire.endTransmission();
  Wire.beginTransmission(AW_ADDR); Wire.write(0x11); Wire.write(0x10); Wire.endTransmission();

  Wire.beginTransmission(AW_ADDR); Wire.write(0x05); 
  Wire.write(0xFF & ~((1<<3) | (1<<4) | (1<<5) | (1<<7))); Wire.endTransmission();

  expander_p1 = PIN_RST | PIN_BL | PIN_AMP;
  writeExpanderPins();

  setBit(PIN_RST, 0); delay(100); setBit(PIN_RST, 1); delay(100);
  writeCommand(0x01); delay(150); writeCommand(0x11); delay(150); writeCommand(0x20); 
  uint8_t colmod = 0x05; writeCommandData(0x3A, &colmod, 1); 
  uint8_t madctl = 0xA8; writeCommandData(0x36, &madctl, 1);
  writeCommand(0x29); setBit(PIN_DC, 1);
}

void showDisplay() {
  uint8_t d_x[] = {0x00, OFFSET_X, 0x00, (uint8_t)(127 + OFFSET_X)}; writeCommandData(0x2A, d_x, 4);
  uint8_t d_y[] = {0x00, OFFSET_Y, 0x00, (uint8_t)(127 + OFFSET_Y)}; writeCommandData(0x2B, d_y, 4);
  writeCommand(0x2C); 
  digitalWrite(TFT_CS, LOW); setBit(PIN_DC, 1);
  SPI.writeBytes((uint8_t*)fb, 128 * 128 * 2);
  digitalWrite(TFT_CS, HIGH);
}

//Video Decoder (ripped from the python version that was too heavy to run audio and video at the same time)
void render_full_1x(uint8_t* data, int n) {
  for (int i = 0; i < n; i++) {
    for (int bit = 0; bit < 8; bit++) fb[i * 8 + bit] = (data[i] & (0x80 >> bit)) ? 0xFFFF : 0x0000;
  }
}


//tbh i didnt even know how this works. I had to shrink the video to 1.2 mb so it would fit. I guess credits to Claude for this function????
void render_delta_rle_1x(uint8_t* stream, int stream_len) {
  int stream_pos = 0; int block_idx = 0;
  while (stream_pos < stream_len) {
    uint8_t cmd = stream[stream_pos++];
    if (cmd < 0x40) { block_idx += (cmd + 1); } 
    else if (cmd < 0x80) {
      int run = (cmd & 0x3F) + 1;
      for (int i = 0; i < run; i++) {
        int dst_x = (block_idx % 32) * 4; int dst_y = (block_idx / 32) * 4;
        for (int r = 0; r < 4; r++) {
          int idx = (dst_y + r) * 128 + dst_x;
          for (int c = 0; c < 4; c++) fb[idx++] = 0x0000;
        }
        block_idx++;
      }
    } else if (cmd < 0xC0) {
      int run = (cmd & 0x3F) + 1;
      for (int i = 0; i < run; i++) {
        int dst_x = (block_idx % 32) * 4; int dst_y = (block_idx / 32) * 4;
        for (int r = 0; r < 4; r++) {
          int idx = (dst_y + r) * 128 + dst_x;
          for (int c = 0; c < 4; c++) fb[idx++] = 0xFFFF;
        }
        block_idx++;
      }
    } else {
      uint8_t mask = cmd & 0x0F;
      int num_rows = (mask & 8?1:0) + (mask & 4?1:0) + (mask & 2?1:0) + (mask & 1?1:0);
      uint16_t payload_val = (num_rows <= 2) ? (stream[stream_pos++] << 8) : ((stream[stream_pos++] << 8) | stream[stream_pos++]);
      int dst_x = (block_idx % 32) * 4; int dst_y = (block_idx / 32) * 4;
      int nibble_shift = 12;
      for (int r = 0; r < 4; r++) {
        if (mask & (1 << (3 - r))) {
          uint8_t row_data = (payload_val >> nibble_shift) & 0x0F;
          nibble_shift -= 4;
          int idx = (dst_y + r) * 128 + dst_x;
          for (int c = 0; c < 4; c++) fb[idx++] = (row_data & (1 << (3 - c))) ? 0xFFFF : 0x0000;
        }
      }
      block_idx++;
    }
  }
}

//Lyrics Parser. This parser is Hot dogshit. I synced the lyrics myself and something went wrong. I was too lazy redoing it. 
void parseLRC() {
  File f = LittleFS.open("/lyrics.lrc", "r");
  if (!f) { Serial.println("no lyrics.lrc"); return; }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();

    if (line.length() < 9 || line[0] != '[') continue;
    int mins = line.substring(1, 3).toInt();
    float secs = line.substring(4, 9).toFloat();
    int line_ms = mins * 60000 + (int)(secs * 1000);

    String rest = line.substring(10);

    LyricLine lyric_line;
    lyric_line.line_ms = line_ms;

    int pos = 0;
    std::vector<int> tag_times;
    std::vector<String> word_texts;

    while (pos < (int)rest.length()) {
      if (rest[pos] == '<') {
        int end = rest.indexOf('>', pos);
        if (end == -1) break;
        int tm = rest.substring(pos+1, pos+3).toInt();
        float ts = rest.substring(pos+4, pos+9).toFloat();
        tag_times.push_back(tm * 60000 + (int)(ts * 1000));
        pos = end + 1;
      } else {
        int next_tag = rest.indexOf('<', pos);
        String word;
        if (next_tag == -1) {
          word = rest.substring(pos);
          pos = rest.length();
        } else {
          word = rest.substring(pos, next_tag);
          pos = next_tag;
        }
        word.trim();
        if (word.length() > 0) {
          word_texts.push_back(word);
        }
      }
    }

    //Split last word (my shitty lyrics syncer destroied information)
    if (!word_texts.empty()) {
      String last = word_texts.back();
      int sp = last.indexOf(' ');
      if (sp != -1) {
        word_texts.pop_back();
        word_texts.push_back(last.substring(0, sp));
        word_texts.push_back(last.substring(sp + 1));
      }
    }

    for (int i = 0; i < (int)word_texts.size(); i++) {
      LyricWord w;
      w.text = word_texts[i];
      w.ms = (i == 0) ? line_ms : (i-1 < (int)tag_times.size() ? tag_times[i-1] : line_ms);
      lyric_line.words.push_back(w);
    }

    if (!lyric_line.words.empty())
      lyrics.push_back(lyric_line);
  }
  f.close();
  Serial.printf("Lyrics loaded: %d lines\n", lyrics.size());
}

//Playerrrrr
void playVideo() {
  current_lyric_line = -1;
  current_lyric_word = -1;
  unsigned long video_start_ms = millis();
  File videoFile = LittleFS.open("/frames.bin", "r");
  if (!videoFile) { Serial.println("Video is missing."); return; }
  uint8_t* buffer = (uint8_t*)malloc(bytes_per_frame);
  
  audio_idx = 44; //Reverse audio 
  play_audio = true; 
  
  wakeHardware(); //Turn on annoying speaker

  while (videoFile.available()) {
    unsigned long t_start = millis();
    int header = videoFile.read();
    if (header == -1) break;
    
    if (header == 0xFF) {
      videoFile.read(buffer, bytes_per_frame); render_full_1x(buffer, bytes_per_frame);
    } else if (header == 0x00) {
      uint8_t len_bytes[2]; videoFile.read(len_bytes, 2);
      int stream_len = len_bytes[0] | (len_bytes[1] << 8);
      videoFile.read(buffer, stream_len); render_delta_rle_1x(buffer, stream_len);
    }
    showDisplay();

    //Lyrics outout (serial)
    {
      int now_ms = (int)(millis() - video_start_ms);
      int start_li = (current_lyric_line >= 0) ? current_lyric_line : 0;
      for (int li = start_li; li < (int)lyrics.size(); li++) {
        if (now_ms >= lyrics[li].line_ms) {
          if (li > current_lyric_line) {
            current_lyric_line = li;
            current_lyric_word = 0;
            Serial.println(); //new line at ne line ?
            Serial.print(lyrics[li].words[0].text);
            Serial.print(" ");
          }
          auto& words = lyrics[li].words;
          for (int wi = current_lyric_word + 1; wi < (int)words.size(); wi++) {
            if (now_ms >= words[wi].ms) {
              Serial.print(words[wi].text);
              Serial.print(" ");
              current_lyric_word = wi;
            } else break;
          }
        } else break;
      }
    }

    if (isButtonAPressed()) {
      Serial.println("Cancelled (button press)");
      break;
    }

    unsigned long elapsed = millis() - t_start;
    if (elapsed < frame_ms) delay(frame_ms - elapsed); else yield(); 
  }
  
  play_audio = false; 
  videoFile.close();
  free(buffer);
  
  for(int i=0; i<16384; i++) fb[i] = 0x0000;
  showDisplay();

  sleepHardware();//Disable sound and backlight
  Serial.println("Press B to start");
}

//main
void setup() {
  WiFi.mode(WIFI_OFF); //idk if this is even doing something. I tried to minimize the speaker noise.
  Serial.begin(115200);
  delay(1000);
  Serial.println("Hello World!");

  LittleFS.begin(true);
  parseLRC();

  File audioFile = LittleFS.open("/audio.wav", "r");
  if (audioFile) {
    audio_size = audioFile.size();
    audio_buffer = (uint8_t*)ps_malloc(audio_size); 
    if (audio_buffer != NULL) audioFile.read(audio_buffer, audio_size);
    audioFile.close();
  }

  File infoFile = LittleFS.open("/frames_info.txt", "r");
  if (infoFile) {
    while (infoFile.available()) {
      String line = infoFile.readStringUntil('\n'); int idx = line.indexOf('=');
      if (idx != -1) {
        String key = line.substring(0, idx); String val = line.substring(idx + 1);
        key.trim(); val.trim();
        if (key == "fps") fps = val.toInt();
        if (key == "bytes_per_frame") bytes_per_frame = val.toInt();
      }
    }
    infoFile.close();
  }
  frame_ms = 1000 / fps;

  initSystem();
  
  //Go to sleep
  for(int i=0; i<16384; i++) fb[i] = 0x0000;
  showDisplay();
  sleepHardware(); //wait till B pressed

  if (audio_buffer != NULL) {
    #if ESP_ARDUINO_VERSION_MAJOR >= 3
      audio_timer = timerBegin(1000000); 
      timerAttachInterrupt(audio_timer, &onAudioTimer);
      timerAlarm(audio_timer, 91, true, 0); 
    #else
      audio_timer = timerBegin(0, 80, true); 
      timerAttachInterrupt(audio_timer, &onAudioTimer, true);
      timerAlarmWrite(audio_timer, 91, true); 
      timerAlarmEnable(audio_timer);
    #endif
  }

  Serial.println("Ready. Press B to play.");
}

void loop() {
  if (isButtonBPressed()) {
    Serial.println("Starting Video.");
    playVideo();
    delay(500); 
  }
  delay(50); 
}