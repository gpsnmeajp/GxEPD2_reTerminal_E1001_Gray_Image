// =============================================================================
// GxEPD2_reTerminal_E1001_Gray_Image.ino
// Seeed reTerminal E1001 用  4階調電子ペーパー ホーム表示スケッチ
//
// 概要:
//   - secrets.h に記述した Wi-Fi 認証情報で自宅ネットワークに接続する
//   - 宅内サーバーから 800x480 の 4階調フレームデータ (frame.bin) を HTTP で取得する
//   - UC8179 ドライバの電子ペーパーパネルに取得画像を表示する
//   - 表示後はディープスリープに入り、6時間後またはユーザーキー押下で復帰する
//
// フレームフォーマット (サーバーから受信するバイナリ):
//   800 x 480 ピクセル, 1ピクセル = 2ビット, 1バイトに 4ピクセル格納
//   バイト内ピクセル順: 左→右 = ビット 7..6, 5..4, 3..2, 1..0
//   階調値: 0=黒, 1=ダークグレー, 2=ライトグレー, 3=白
//
// ユーザーキー配線 (同梱の回路図参照):
//   KEY0 = GPIO3, KEY1 = GPIO4, KEY2 = GPIO5
//   すべてアクティブローで内部プルアップ使用
//
// 機密情報 (Wi-Fi SSID/PASS, サーバーアドレス) は secrets.h で管理
// secrets.h は .gitignore によりリポジトリには含まれない
// secrets.h.example を参考に secrets.h を作成すること
// =============================================================================

// --- 標準ライブラリ / ESP32 フレームワーク ---
#include <Arduino.h>          // Arduino 基本 API
#include <WiFi.h>              // ESP32 Wi-Fi ドライバ
#include <WiFiClient.h>        // TCP クライアント
#include <SPI.h>               // SPI バス制御
#include <esp_sleep.h>         // ESP32 ディープスリープ API

// --- Adafruit GFX (フォント・描画) ---
#include <Adafruit_GFX.h>                    // 抽象グラフィックスクラス
#include <Fonts/FreeMonoBold9pt7b.h>         // 等幅ボールド 9pt
#include <Fonts/FreeMonoBold12pt7b.h>        // 等幅ボールド 12pt
#include <Fonts/FreeSansBold18pt7b.h>        // サンセリフボールド 18pt
#include <Fonts/FreeSansBold12pt7b.h>        // サンセリフボールド 12pt

// --- 機密情報 (Wi-Fi / サーバー設定) ---
// secrets.h は .gitignore によりリポジトリに含まれない
// secrets.h.example をコピーして値を書き換えること
#include "secrets.h"

// ===== 動作タイミング設定 =====
// ディープスリープ間隔: 6時間 (マイクロ秒単位)
static const uint64_t SLEEP_INTERVAL_US       = 6ULL * 60ULL * 60ULL * 1000000ULL;
// Wi-Fi 接続タイムアウト (ミリ秒)
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
// HTTP 通信タイムアウト (ミリ秒)
static const uint32_t HTTP_TIMEOUT_MS         = 15000;

// ===== ピン配置 (reTerminal E1001) =====
// 電子ペーパーパネルとの SPI 接続ピン
#define EPD_SCK_PIN   7    // SPI クロック
#define EPD_MOSI_PIN  9    // SPI データ出力 (MOSI)
#define EPD_CS_PIN    10   // SPI チップセレクト (アクティブ LOW)
#define EPD_DC_PIN    11   // データ/コマンド 選択 (HIGH=データ, LOW=コマンド)
#define EPD_RES_PIN   12   // ハードウェアリセット (アクティブ LOW)
#define EPD_BUSY_PIN  13   // ビジー信号: パネル処理中は LOW

// ユーザーキー入力ピン (アクティブロー, 内部プルアップ使用)
#define KEY0_PIN      3
#define KEY1_PIN      4
#define KEY2_PIN      5

// ===== 画面サイズ定義 =====
#define EPD_W         800                    // 横ピクセル数
#define EPD_H         480                    // 縦ピクセル数
#define FRAME_BYTES   (EPD_W * EPD_H / 4)   // 1ピクセル 2ビット→ 96,000 バイト

// ディープスリープからの復帰を許可するボタンの EXT1 ビットマスク
#define WAKE_BUTTON_MASK  ((1ULL << KEY0_PIN) | (1ULL << KEY1_PIN) | (1ULL << KEY2_PIN))

// HSPI バスを使用 (ESP32 の SPI2 パーリファル)
SPIClass hspi(HSPI);
// SPI 設定: 2 MHz, MSB ファースト, モード 0
SPISettings spiSet(2000000, MSBFIRST, SPI_MODE0);

// ===== 階調値の定義 =====
#define G_BLACK       0   // 黒
#define G_DARK_GRAY   1   // ダークグレー
#define G_LIGHT_GRAY  2   // ライトグレー
#define G_WHITE       3   // 白

// =============================================================================
// UC8179 4階調表示用 LUT (Look-Up Table)
// ドライバ IC に書き込む波形制御テーブル。階調遷移ごとに電圧パターンを指定する。
// Seeed_GFX の UC8179_Defines.h の値を使用。
// =============================================================================

// VCOM (共通電極) の LUT
static const uint8_t LUT_VCOM_GRAY[] = {
  0x00,0x00,0x06,0x08,0x07,0x01,
  0x00,0x06,0x0A,0x0B,0x0A,0x01,
  0x00,0x03,0x03,0x00,0x00,0x03,
  0x00,0x05,0x09,0x06,0x06,0x01,
  0x00,0x02,0x02,0x0A,0x0A,0x01,
  0x00,0x0A,0x11,0x06,0x07,0x01,
  0x00,0x02,0x01,0x02,0x01,0x01,
};

// 白→白 遷移の LUT
static const uint8_t LUT_WW_GRAY[] = {
  0x15,0x00,0x06,0x08,0x07,0x01,
  0x54,0x06,0x0A,0x0B,0x0A,0x01,
  0x90,0x03,0x03,0x00,0x00,0x03,
  0x2A,0x05,0x09,0x06,0x06,0x01,
  0xAA,0x02,0x02,0x0A,0x0A,0x01,
  0x00,0x0A,0x11,0x06,0x07,0x01,
  0x28,0x02,0x01,0x02,0x01,0x01,
};

// 黒→白 遷移の LUT
static const uint8_t LUT_KW_GRAY[] = {
  0x2A,0x00,0x06,0x08,0x07,0x01,
  0x59,0x06,0x0A,0x0B,0x0A,0x01,
  0x90,0x03,0x03,0x00,0x00,0x03,
  0x5A,0x05,0x09,0x06,0x06,0x01,
  0xA8,0x02,0x02,0x0A,0x0A,0x01,
  0x45,0x0A,0x11,0x06,0x07,0x01,
  0xA8,0x02,0x01,0x02,0x01,0x01,
};

// 白→黒 遷移の LUT
static const uint8_t LUT_WK_GRAY[] = {
  0x16,0x00,0x06,0x08,0x07,0x01,
  0xA0,0x06,0x0A,0x0B,0x0A,0x01,
  0x90,0x03,0x03,0x00,0x00,0x03,
  0x99,0x05,0x09,0x06,0x06,0x01,
  0xA0,0x02,0x02,0x0A,0x0A,0x01,
  0x40,0x0A,0x11,0x06,0x07,0x01,
  0x20,0x02,0x01,0x02,0x01,0x01,
};

// 黒→黒 遷移の LUT
static const uint8_t LUT_KK_GRAY[] = {
  0x26,0x00,0x06,0x08,0x07,0x01,
  0x6A,0x06,0x0A,0x0B,0x0A,0x01,
  0x90,0x03,0x03,0x00,0x00,0x03,
  0x65,0x05,0x09,0x06,0x06,0x01,
  0x50,0x02,0x02,0x0A,0x0A,0x01,
  0x10,0x0A,0x11,0x06,0x07,0x01,
  0x10,0x02,0x01,0x02,0x01,0x01,
};

// パネル初期化コマンドの追加パラメータ
// [0]:VCOM電圧, [1..3]:パネルサイズ/設定, [4]:PLLクロック, [5]:VCOM DC
static const uint8_t CMD_USER_GRAY[] = {
  0x17, 0x3F, 0x3F, 0x07, 0x06, 0x12,
};

// =============================================================================
// Gray4Canvas クラス
// Adafruit_GFX を継承した 4階調フレームバッファ。
// 1ピクセル = 2ビット (0、3) でヒープメモリに格納する。
// drawPixel を実装することで GFX の全描画関数が利用可能になる。
// =============================================================================
class Gray4Canvas : public Adafruit_GFX
{
public:
  // コンストラクタ: 幅・高さを指定してキャンバスを作成
  Gray4Canvas(uint16_t w, uint16_t h) : Adafruit_GFX(w, h), _buf(nullptr) {}

  // フレームバッファをヒープに確保する (96,000 バイト)
  // 確保成功: true, 失敗: false
  bool begin()
  {
    _buf = (uint8_t*)malloc(FRAME_BYTES);
    if (_buf) memset(_buf, 0xFF, FRAME_BYTES); // 全面白で初期化
    return _buf != nullptr;
  }

  // Adafruit_GFX の純粋仮想関数をオーバーライド
  // (x, y) のピクセルを color (0、3) で描画する
  // rotation 設定に応じて座標を変換してからバッファに書き込む
  void drawPixel(int16_t x, int16_t y, uint16_t color) override
  {
    if (!_buf) return;
    if (x < 0 || x >= width() || y < 0 || y >= height()) return;

    // 回転角に応じて実際のバッファ座標に変換
    int16_t rx = x, ry = y;
    switch (getRotation()) {
      case 1: rx = _width - 1 - y; ry = x; break;               // 90度回転
      case 2: rx = _width - 1 - x; ry = _height - 1 - y; break; // 180度回転
      case 3: rx = y; ry = _height - 1 - x; break;              // 270度回転
    }

    // 2ビット/ピクセル形式でバッファに書き込む
    // バイトインデックス: (ry * 幅/4) + rx/4
    // シフト量: (3 - rx%4) * 2  (左から右へ bits 7..6, 5..4, 3..2, 1..0)
    uint8_t g = color & 0x03;
    uint32_t idx = uint32_t(ry) * (_width / 4) + rx / 4;
    uint8_t shift = (3 - (rx & 3)) * 2;
    _buf[idx] = (_buf[idx] & ~(0x03 << shift)) | (g << shift);
  }

  // 全画面を指定の階調で塩つぶす
  void fillScreen(uint16_t color) override
  {
    if (!_buf) return;
    uint8_t g = color & 0x03;
    // 1バイトに同じ階調を 4ピクセル分詰めてメモリセット
    uint8_t fill = (g << 6) | (g << 4) | (g << 2) | g;
    memset(_buf, fill, FRAME_BYTES);
  }

  // 生フレームバッファへのポインタを返す
  uint8_t* buffer() { return _buf; }

private:
  uint8_t* _buf; // フレームバッファ (ヒープ上に確保)
};

// グローバルキャンバスインスタンス (800x480)
Gray4Canvas canvas(EPD_W, EPD_H);

// =============================================================================
// SPI / UC8179 低レベル通信関数
// =============================================================================

// BUSY ピンが HIGH になるまでポーリング待機する
// パネルが前のコマンドを処理している間は LOW を維持する
void checkBusy()
{
  delay(10);
  while (!digitalRead(EPD_BUSY_PIN)) delay(10);
}

// UC8179 へコマンドバイトを 1 バイト送信する
// DC ピンを LOW にしてコマンドモードで転送する
void writeCommand(uint8_t cmd)
{
  hspi.beginTransaction(spiSet);
  digitalWrite(EPD_DC_PIN, LOW);   // コマンドモード
  digitalWrite(EPD_CS_PIN, LOW);
  hspi.transfer(cmd);
  digitalWrite(EPD_CS_PIN, HIGH);
  digitalWrite(EPD_DC_PIN, HIGH);  // データモードに戻す
  hspi.endTransaction();
}

// UC8179 へデータバイトを 1 バイト送信する
// DC ピンは HIGH (データモード) のまま転送する
void writeData(uint8_t data)
{
  hspi.beginTransaction(spiSet);
  digitalWrite(EPD_CS_PIN, LOW);
  hspi.transfer(data);
  digitalWrite(EPD_CS_PIN, HIGH);
  hspi.endTransaction();
}

// LUT テーブルをコマンド付きで送信するヘルパー関数
void writeLUT(uint8_t cmd, const uint8_t* lut, uint16_t len)
{
  writeCommand(cmd);
  for (uint16_t i = 0; i < len; i++) writeData(lut[i]);
}

// =============================================================================
// initGrayMode()
// UC8179 を 4階調表示モードで初期化する
// リセット → 電源設定 → PLL → VCOM DC → ブースター → 電源 ON
// → パネル設定 → 解像度設定 → LUT 書き込み
// =============================================================================
void initGrayMode()
{
  // ハードウェアリセット (10ms LOW → HIGH)
  digitalWrite(EPD_RES_PIN, LOW); delay(10);
  digitalWrite(EPD_RES_PIN, HIGH); delay(10);
  checkBusy();

  // 0x01: 電源設定 (PWR) — VGH/VGL および VSH/VSL の電圧を設定する
  writeCommand(0x01);
  writeData(0x07);
  writeData(CMD_USER_GRAY[0]); // 0x17
  writeData(CMD_USER_GRAY[1]); // 0x3F
  writeData(CMD_USER_GRAY[2]); // 0x3F
  writeData(CMD_USER_GRAY[3]); // 0x07

  // 0x30: PLL 制御 — クロック周波数を設定する
  writeCommand(0x30);
  writeData(CMD_USER_GRAY[4]); // 0x06

  // 0x82: VCOM DC 設定
  writeCommand(0x82);
  writeData(CMD_USER_GRAY[5]); // 0x12

  // 0x06: ブースター ソフトスタート設定
  writeCommand(0x06);
  writeData(0x27);
  writeData(0x27);
  writeData(0x28);
  writeData(0x17);

  // 0x04: 電源 ON
  writeCommand(0x04);
  delay(100);
  checkBusy(); // 電源安定まで待機

  // 0x00: パネル設定 (PSR) — KW/KWR モード、スキャン方向等を設定
  writeCommand(0x00);
  writeData(0x3F);

  // 0xE3: 電力セービング設定
  writeCommand(0xE3);
  writeData(0x88);

  // 0x50: VCOM とデータ インターバル設定
  writeCommand(0x50);
  writeData(0x10);
  writeData(0x07);

  // 0x52: T_VDS_OFF 設定
  writeCommand(0x52);
  writeData(0x00);

  // 0x61: 解像度設定 (800x480)
  writeCommand(0x61);
  writeData(EPD_W >> 8);    // 幅の上位バイト
  writeData(EPD_W & 0xFF);  // 幅の下位バイト
  writeData(EPD_H >> 8);    // 高さの上位バイト
  writeData(EPD_H & 0xFF);  // 高さの下位バイト

  // LUT 書き込み (0x20～0x24): 各階調遷移の波形テーブル
  writeLUT(0x20, LUT_VCOM_GRAY, sizeof(LUT_VCOM_GRAY)); checkBusy();
  writeLUT(0x21, LUT_WW_GRAY,   sizeof(LUT_WW_GRAY));   checkBusy();
  writeLUT(0x22, LUT_KW_GRAY,   sizeof(LUT_KW_GRAY));   checkBusy();
  writeLUT(0x23, LUT_WK_GRAY,   sizeof(LUT_WK_GRAY));
  writeLUT(0x24, LUT_KK_GRAY,   sizeof(LUT_KK_GRAY));
}

// =============================================================================
// uploadGray4Frame()
// Gray4Canvas のフレームバッファを UC8179 の SRAM に転送する
//
// UC8179 は旧データ (0x10) と新データ (0x13) の 2 面バッファを持ち、
// 差分駆動で高品質な更新を行う。
// 4階調は 1ビット x 2面 で表現される:
//   Gray 値   旧面 (0x10)  新面 (0x13)
//     3 (白)        1           1
//     2 (ライト)      0           1
//     1 (ダーク)      1           0
//     0 (黒)        0           0
// バッファ値 (0=黒) を反転 (3 - gray) して論理値を作成する
// =============================================================================
void uploadGray4Frame()
{
  const uint32_t bytesPerRow = EPD_W / 4; // 1行あたりのバッファバイト数

  // --- 旧データ面 (0x10): gray の bit0 ---
  writeCommand(0x10);
  hspi.beginTransaction(spiSet);
  digitalWrite(EPD_CS_PIN, LOW);
  for (uint16_t row = 0; row < EPD_H; row++) {
    const uint8_t* rp = canvas.buffer() + uint32_t(row) * bytesPerRow;
    // 8 ピクセルを 1 バイトにまとめて転送
    for (uint16_t col8 = 0; col8 < EPD_W / 8; col8++) {
      uint8_t out = 0;
      for (uint8_t bit = 0; bit < 8; bit++) {
        uint16_t px = col8 * 8 + bit;
        uint32_t idx = px / 4;
        uint8_t shift = (3 - (px & 3)) * 2;
        uint8_t gray = 3 - ((rp[idx] >> shift) & 0x03); // 階調値を反転
        if (gray & 0x01) out |= (0x80 >> bit);           // bit0 を出力ビットに設定
      }
      hspi.transfer(out);
    }
  }
  digitalWrite(EPD_CS_PIN, HIGH);
  hspi.endTransaction();

  // --- 新データ面 (0x13): gray の bit1 ---
  writeCommand(0x13);
  hspi.beginTransaction(spiSet);
  digitalWrite(EPD_CS_PIN, LOW);
  for (uint16_t row = 0; row < EPD_H; row++) {
    const uint8_t* rp = canvas.buffer() + uint32_t(row) * bytesPerRow;
    for (uint16_t col8 = 0; col8 < EPD_W / 8; col8++) {
      uint8_t out = 0;
      for (uint8_t bit = 0; bit < 8; bit++) {
        uint16_t px = col8 * 8 + bit;
        uint32_t idx = px / 4;
        uint8_t shift = (3 - (px & 3)) * 2;
        uint8_t gray = 3 - ((rp[idx] >> shift) & 0x03); // 階調値を反転
        if (gray & 0x02) out |= (0x80 >> bit);           // bit1 を出力ビットに設定
      }
      hspi.transfer(out);
    }
  }
  digitalWrite(EPD_CS_PIN, HIGH);
  hspi.endTransaction();
}

// 表示更新を開始し、完了まで待機する
// 0x12 コマンドで更新トリガをかけ、BUSY 解除までポーリングする
void refreshDisplay()
{
  unsigned long t0 = millis();
  writeCommand(0x12); // DISPLAY REFRESH
  delay(100);
  checkBusy();
  Serial.printf("[EPD] refresh %lu ms\n", millis() - t0);
}

// パネルをディープスリープに移行させる
// 0x02: 電源 OFF → 0x07 + 0xA5: スリープ (VCOM 保持)
void sleepDisplay()
{
  writeCommand(0x02); // 電源 OFF
  checkBusy();
  writeCommand(0x07); // ディープスリープ
  writeData(0xA5);    // チェックコード (UC8179 仕様固定値)
}

// =============================================================================
// drawCenteredText()
// 指定フォントで文字列を水平中央揃えで描画する
// text : 表示する文字列
// y    : ベースライン Y 座標
// font : GFX フォントポインタ
// gray : 描画色 (0～3)
// =============================================================================
void drawCenteredText(const char* text, int16_t y, const GFXfont* font, uint8_t gray)
{
  canvas.setFont(font);
  canvas.setTextColor(gray);
  int16_t tbx, tby;
  uint16_t tbw, tbh;
  canvas.getTextBounds(text, 0, 0, &tbx, &tby, &tbw, &tbh);
  canvas.setCursor((EPD_W - tbw) / 2 - tbx, y);
  canvas.print(text);
}

// =============================================================================
// showLocalMessage()
// Wi-Fi 接続失敗やダウンロードエラー時にローカルで生成したメッセージを表示する
// line1: メインメッセージ (大文字)
// line2: サブメッセージ (小文字)
// =============================================================================
void showLocalMessage(const char* line1, const char* line2)
{
  canvas.fillScreen(G_WHITE);
  canvas.fillRect(0, 0, EPD_W, 56, G_BLACK); // 上部ヘッダーバー
  drawCenteredText("E1001 Home Display", 38, &FreeMonoBold12pt7b, G_WHITE);
  canvas.setFont(&FreeSansBold18pt7b);
  canvas.setTextColor(G_BLACK);
  canvas.setCursor(52, 180);
  canvas.print(line1);
  canvas.setFont(&FreeSansBold12pt7b);
  canvas.setCursor(52, 235);
  canvas.print(line2);
  canvas.setFont(&FreeMonoBold9pt7b);
  canvas.setCursor(52, 390);
  canvas.print("Next wake: 6 hours or KEY0/KEY1/KEY2"); // 次回復帰案内
  uploadGray4Frame();
  refreshDisplay();
}

// =============================================================================
// Wi-Fi 接続・切断関数
// =============================================================================

// WIFI_CONNECT_TIMEOUT_MS 内に接続できた場合 true を返す
bool connectWiFi()
{
  Serial.printf("[WiFi] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);           // ステーションモード (アクセスポイントには接続しない)
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("[WiFi] timeout");
      return false;
    }
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[WiFi] IP ");
  Serial.println(WiFi.localIP());
  return true;
}

// Wi-Fi を切断し、無線モジュールを OFF にする (消費電力削減)
void stopWiFi()
{
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// =============================================================================
// readExact()
// TCP ストリームから正確に total バイト読み取る
// タイムアウト (HTTP_TIMEOUT_MS) または接続断で途中終了した場合 false を返す
// =============================================================================
bool readExact(WiFiClient& client, uint8_t* out, size_t total)
{
  size_t got = 0;
  uint32_t lastData = millis();
  while (got < total) {
    int available = client.available();
    if (available > 0) {
      size_t want = min((size_t)available, total - got);
      int n = client.read(out + got, want);
      if (n > 0) {
        got += (size_t)n;
        lastData = millis();
      }
    } else {
      if (!client.connected() && !client.available()) break; // 接続断
      if (millis() - lastData > HTTP_TIMEOUT_MS) break;       // タイムアウト
      delay(2);
    }
  }

  Serial.printf("[HTTP] read %u/%u bytes\n", (unsigned)got, (unsigned)total);
  return got == total;
}

// =============================================================================
// downloadFrame()
// HTTP GET /frame.bin を発行し、FRAME_BYTES バイトをキャンバスバッファに受信する
// 成功: true、接続失敗・ステータスエラー・サイズ不一致: false
// =============================================================================
bool downloadFrame()
{
  WiFiClient client;
  client.setTimeout(HTTP_TIMEOUT_MS);

  Serial.printf("[HTTP] connecting to %s:%u\n", SERVER_HOST, SERVER_PORT);
  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    Serial.println("[HTTP] connect failed");
    return false;
  }

  // HTTP/1.1 GET リクエスト送信
  client.print(String("GET ") + SERVER_PATH + " HTTP/1.1\r\n" +
               "Host: " + SERVER_HOST + "\r\n" +
               "User-Agent: E1001-Gray4\r\n" +
               "Accept: application/octet-stream\r\n" +
               "Connection: close\r\n\r\n");

  // ステータスラインの確認
  String status = client.readStringUntil('\n');
  status.trim();
  Serial.print("[HTTP] ");
  Serial.println(status);
  if (!status.startsWith("HTTP/1.1 200") && !status.startsWith("HTTP/1.0 200")) {
    client.stop();
    return false;
  }

  // レスポンスヘッダーを読み飛ばし、Content-Length を取得する
  int32_t contentLength = -1;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break; // 空行 = ヘッダー終端

    int colon = line.indexOf(':');
    if (colon > 0) {
      String name = line.substring(0, colon);
      String value = line.substring(colon + 1);
      name.toLowerCase();
      value.trim();
      if (name == "content-length") {
        contentLength = value.toInt();
      }
    }
  }

  // Content-Length が期待値と異なる場合は拒否
  if (contentLength != -1 && contentLength != FRAME_BYTES) {
    Serial.printf("[HTTP] unexpected Content-Length: %ld\n", (long)contentLength);
    client.stop();
    return false;
  }

  // フレームデータをキャンバスバッファに直接受信する
  bool ok = readExact(client, canvas.buffer(), FRAME_BYTES);
  client.stop();
  return ok;
}

// =============================================================================
// enterDeepSleep()
// 表示完了後にシステムをディープスリープに移行する
// 復帰条件:
//   1. タイマー: SLEEP_INTERVAL_US (6時間) 経過
//   2. EXT1 割り込み: KEY0/KEY1/KEY2 いずれかを押下 (アクティブロー)
// =============================================================================
void enterDeepSleep()
{
  Serial.println("[Sleep] display off");
  sleepDisplay(); // 電子ペーパーをスリープモードへ
  stopWiFi();     // Wi-Fi 無線を OFF

  // ウェイクアップ用ボタンピンをプルアップ入力に設定
  pinMode(KEY0_PIN, INPUT_PULLUP);
  pinMode(KEY1_PIN, INPUT_PULLUP);
  pinMode(KEY2_PIN, INPUT_PULLUP);

  // ウェイクアップソースを設定
  esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_US);                          // タイマー復帰
  esp_sleep_enable_ext1_wakeup(WAKE_BUTTON_MASK, ESP_EXT1_WAKEUP_ANY_LOW);  // ボタン復帰

  Serial.println("[Sleep] 6h timer or KEY0/KEY1/KEY2 wake");
  Serial.flush();
  esp_deep_sleep_start(); // ディープスリープ開始 (以降の処理は実行されない)
}

// =============================================================================
// setup()
// ESP32 の起動時 (ディープスリープ復帰時も含む) に 1 回だけ実行される
// =============================================================================
void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[E1001] boot");

  // ウェイクアップ原因を確認してログ出力 (デバッグ用)
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("[Wake] cause=%d\n", (int)cause);

  // ユーザーキーをプルアップ入力に設定
  pinMode(KEY0_PIN, INPUT_PULLUP);
  pinMode(KEY1_PIN, INPUT_PULLUP);
  pinMode(KEY2_PIN, INPUT_PULLUP);

  // 電子ペーパーパネルの制御ピンを初期化
  pinMode(EPD_CS_PIN, OUTPUT);  digitalWrite(EPD_CS_PIN, HIGH);  // CS: 非選択
  pinMode(EPD_DC_PIN, OUTPUT);  digitalWrite(EPD_DC_PIN, HIGH);  // DC: データモード
  pinMode(EPD_RES_PIN, OUTPUT); digitalWrite(EPD_RES_PIN, HIGH); // RES: 非リセット
  pinMode(EPD_BUSY_PIN, INPUT);                                   // BUSY: 入力

  // HSPI バスを初期化 (SCK=7, MISO=未使用=-1, MOSI=9, SS=未使用=-1)
  hspi.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, -1);

  // フレームバッファをヒープに確保
  if (!canvas.begin()) {
    Serial.println("[E1001] frame buffer allocation failed");
    delay(5000);
    esp_deep_sleep_start(); // 確保失敗時はスリープへ (無限リセットを防止)
  }

  // パネルを 4階調モードで初期化
  initGrayMode();

  // Wi-Fi 接続とフレームダウンロードを実行
  bool ok = connectWiFi() && downloadFrame();
  if (ok) {
    // 成功: サーバーから受信したフレームを表示
    Serial.println("[E1001] frame downloaded");
    uploadGray4Frame();
    refreshDisplay();
  } else {
    // 失敗: エラーメッセージをローカルで生成して表示
    Serial.println("[E1001] showing local error");
    showLocalMessage("Image download failed", "Check Wi-Fi and server");
  }

  // 表示完了後にディープスリープへ移行
  enterDeepSleep();
}

// loop() は使用しない
// setup() の最後で esp_deep_sleep_start() が呼ばれるため、
// loop() に到達することはない
void loop()
{
}
