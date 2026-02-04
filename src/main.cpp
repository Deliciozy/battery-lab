#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#include <FirebaseClient.h>

// ========= Wi-Fi & Firebase =========
const char *ssid = "UW MPSK";
const char *password = "UzYVMgPkUnv7FtPk";

#define DATABASE_URL  "https://marry-5f841-default-rtdb.firebaseio.com/"
#define API_KEY       "AIzaSyAO3fpskdSalzh74058P8CeznKwMiodpOk"
#define USER_EMAIL    "hbjmczy@gmail.com"
#define USER_PASSWORD "0509Mary!"

UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD);
FirebaseApp app;

WiFiClientSecure ssl;
using AsyncClient = AsyncClientClass;
AsyncClient asyncClient(ssl);
RealtimeDatabase Database;

// ========= Power Policy Parameters =========
#define SLEEP_SEC              2        // 默认 2 秒唤醒一次（可根据功耗再加大）
#define HEARTBEAT_SEC          600      // 10 分钟心跳一次
#define WIFI_TIMEOUT_MS        5000     // Wi-Fi 连接超时 5 秒
#define MEASURE_COUNT          3        // 1-3 次取中位数
#define DETECT_NEAR_CM         50.0f    // 距离 < 50cm 触发事件
#define DELTA_THRESHOLD_CM     15.0f    // 距离变化 > 15cm 触发事件

// ========= Ultrasonic Pins =========
// 注意：请按你实际接线修改 GPIO
const int trigPin = D2;
const int echoPin = D3;
const float soundSpeed = 0.0343f;

// ========= RTC Memory (Deep sleep survives) =========
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR float lastDistanceCm = -1.0f;
RTC_DATA_ATTR uint32_t lastHeartbeatSec = 0; // 用累计秒计更简单

// ========= Forward Decls =========
float measureDistanceOnce();
float measureDistanceMedian3();
bool shouldUploadEvent(float d, bool &eventFlag);
bool connectWiFiShort();
bool firebaseUpload(float d, bool eventFlag, bool isHeartbeat);
void goDeepSleep(uint32_t sec);
void processData(AsyncResult &aResult);

void setup() {
  Serial.begin(115200);
  delay(200);

  bootCount++;

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  // ========= 1) 快速测距（<80ms 目标）=========
  float d = measureDistanceMedian3();
  if (d <= 0 || d >= 500) d = 999.0; // 无回波/异常

  bool eventFlag = false;
  bool uploadNeeded = shouldUploadEvent(d, eventFlag);

  // ========= 2) 心跳判定 =========
  // 这里用 bootCount * SLEEP_SEC 近似累计运行秒（不依赖真实时钟）
  uint32_t nowSec = bootCount * SLEEP_SEC;
  bool heartbeatDue = (lastHeartbeatSec == 0) || (nowSec - lastHeartbeatSec >= HEARTBEAT_SEC);

  // 需要上传：事件触发 或 心跳到期
  bool doUpload = uploadNeeded || heartbeatDue;

  Serial.printf("\nBoot #%lu\n", (unsigned long)bootCount);
  Serial.printf("Distance: %.1f cm\n", d);
  Serial.printf("LastDist: %.1f cm\n", lastDistanceCm);
  Serial.printf("Event?   : %s\n", eventFlag ? "YES" : "NO");
  Serial.printf("HeartbeatDue? %s\n", heartbeatDue ? "YES" : "NO");
  Serial.printf("Upload?  : %s\n", doUpload ? "YES" : "NO");

  // ========= 3) 上传（短联网）=========
  if (doUpload) {
    bool ok = firebaseUpload(d, eventFlag, heartbeatDue && !uploadNeeded);
    Serial.printf("Upload result: %s\n", ok ? "OK" : "FAIL");

    if (heartbeatDue) lastHeartbeatSec = nowSec;
  }

  // ========= 4) 更新 RTC 状态 =========
  if (d < 999.0) lastDistanceCm = d;

  // ========= 5) 立即深睡 =========
  goDeepSleep(SLEEP_SEC);
}

void loop() {}

// ========= 事件触发逻辑 =========
bool shouldUploadEvent(float d, bool &eventFlag) {
  eventFlag = false;

  if (d < DETECT_NEAR_CM) {
    eventFlag = true;
    return true;
  }

  if (lastDistanceCm > 0 && lastDistanceCm < 999.0 && d < 999.0) {
    float delta = fabs(d - lastDistanceCm);
    if (delta > DELTA_THRESHOLD_CM) {
      eventFlag = true;
      return true;
    }
  }

  return false;
}

// ========= 测距：单次 =========
float measureDistanceOnce() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000); // 30ms timeout
  if (duration <= 0) return -1.0f;

  return (float)duration * soundSpeed / 2.0f;
}

// ========= 测距：3 次中位数（抗抖）=========
float measureDistanceMedian3() {
  float a = measureDistanceOnce();
  delay(15);
  float b = measureDistanceOnce();
  delay(15);
  float c = measureDistanceOnce();

  // 过滤无效
  if (a < 0) a = 999.0;
  if (b < 0) b = 999.0;
  if (c < 0) c = 999.0;

  // median of 3
  if ((a <= b && b <= c) || (c <= b && b <= a)) return b;
  if ((b <= a && a <= c) || (c <= a && a <= b)) return a;
  return c;
}

// ========= Wi-Fi 短连接 =========
bool connectWiFiShort() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(ssid, password);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// ========= Firebase 上传（事件/心跳）=========
bool firebaseUpload(float d, bool eventFlag, bool isHeartbeat) {
  if (!connectWiFiShort()) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  ssl.setInsecure();
  ssl.setHandshakeTimeout(5);

  initializeApp(asyncClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  unsigned long authStart = millis();
  while (!app.ready() && millis() - authStart < 6000) {
    app.loop();
    delay(50);
  }
  if (!app.ready()) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  // 上传字段：distance, event, heartbeat, bootCount
  const char* statusStr = eventFlag ? "EVENT" : "NO_EVENT";

  Database.set<float>(asyncClient, "/sensor/distance_cm", d, processData, "dist");
  delay(80); app.loop();

  Database.set<String>(asyncClient, "/sensor/status", statusStr, processData, "status");
  delay(80); app.loop();

  Database.set<int>(asyncClient, "/sensor/bootCount", (int)bootCount, processData, "boot");
  delay(80); app.loop();

  Database.set<bool>(asyncClient, "/sensor/heartbeat", isHeartbeat, processData, "hb");
  delay(80); app.loop();

  // 断网省电
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return true;
}

// ========= 深睡 =========
void goDeepSleep(uint32_t sec) {
  esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
  delay(50);
  esp_deep_sleep_start();
}

void processData(AsyncResult &aResult) {
  if (!aResult.isResult()) return;
  if (aResult.isError()) {
    Serial.printf("Firebase Error: %s\n", aResult.error().message().c_str());
  }
}
