## Power-Saving Strategy (English)

### Scenario
A battery-powered ESP32 device detects whether an object/person moves in front of an ultrasonic sensor in a room and uploads results to Firebase Realtime Database.

### Policy / Rules
1. **Deep sleep dominates:** The ESP32 stays in deep sleep most of the time and wakes up every `SLEEP_SEC` seconds (default: 2 s).
2. **Fast ultrasonic sensing on wake:** Each wake-up performs only **1–3 distance measurements** and uses the **median** value. The sensing duration is kept very short (target < ~80 ms).
3. **Event-triggered upload (minimize Wi-Fi usage):**
   - If the distance becomes **closer than 50 cm**, OR
   - If the distance changes by more than **15 cm** compared to the previous reading,
   the device treats it as a movement/approach event and enables Wi-Fi to upload once.
4. **Heartbeat upload (keep cloud status fresh):** Even without events, the device uploads a heartbeat/status update every `HEARTBEAT_SEC` seconds (e.g., every 10 minutes) so the cloud can confirm the device is alive.
5. **Short network window:** Wi-Fi connection uses a strict timeout (e.g., 5 seconds). After upload success or failure, Wi-Fi is turned off immediately and the ESP32 returns to deep sleep.

### Expected Power Profile
- **Deep sleep stage:** near-zero baseline current.
- **Working stage:** short periodic spikes for wake-up + ultrasonic sensing.
- **Upload stage:** higher current bursts only during Wi-Fi transmission (event/heartbeat).
