// BLE ring connector with status LED.
//
// LED states (onboard blue LED, GPIO13 = LED_G1 in the schematic):
//   SCANNING    slow blink (1 Hz)   - boot, looking for the ring
//   CONNECTING  rapid blink (10 Hz) - ring found, connecting / pairing
//   PAIRED      solid on            - connected and authenticated (bonded)
//
// Pairing uses "Just Works" bonding. If the ring asks for a passkey, see
// onPassKeyRequest() below.

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLESecurity.h>

static const char *TARGET_NAME = "arcxp-1234";
static const uint8_t LED_PIN = 13;

enum LedState : uint8_t { SCANNING, CONNECTING, PAIRED };
static volatile uint8_t ledState = SCANNING;

// Link/pairing progress. Written from BLE callbacks, read in loop().
static volatile bool foundPending = false;   // scan callback saw target
static volatile bool linkDown = false;       // disconnect callback fired
static volatile bool authDone = false;       // pairing finished
static volatile bool authOk = false;

static BLEAdvertisedDevice *target = nullptr;  // copy of the matching advert
static BLEClient *client = nullptr;

// ---------- Scan callback ----------
class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (foundPending || ledState == PAIRED) return;
    if (advertisedDevice.haveName() &&
        advertisedDevice.getName() == TARGET_NAME) {
      Serial.printf("[scan] found \"%s\" addr=%s rssi=%d\r\n", TARGET_NAME,
                    advertisedDevice.getAddress().toString().c_str(),
                    advertisedDevice.getRSSI());
      delete target;
      target = new BLEAdvertisedDevice(advertisedDevice);
      foundPending = true;
    }
  }
};

// ---------- Client (connection) callbacks ----------
class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient *) override {
    Serial.println("[ble] connected");
  }
  void onDisconnect(BLEClient *) override {
    Serial.println("[ble] disconnected");
    linkDown = true;
  }
};

// ---------- Security / pairing callbacks ----------
class SecurityCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    // Only used if the ring requires a passkey instead of Just Works.
    Serial.println("[sec] passkey request from ring; replying 000000");
    return 0;
  }
  void onPassKeyNotify(uint32_t pass_key) override {
    Serial.printf("[ble] passkey: %06lu\r\n", (unsigned long)pass_key);
  }
  bool onConfirmPIN(uint32_t pin) override {
    Serial.printf("[ble] confirm PIN %06lu: yes\r\n", (unsigned long)pin);
    return true;
  }
  bool onSecurityRequest() override {
    Serial.println("[sec] security request from ring; accepting");
    return true;
  }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    authOk = cmpl.success;
    authDone = true;
    Serial.printf("[sec] auth complete: %s  addr=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
                  cmpl.success ? "OK" : "FAILED",
                  cmpl.bd_addr[0], cmpl.bd_addr[1], cmpl.bd_addr[2],
                  cmpl.bd_addr[3], cmpl.bd_addr[4], cmpl.bd_addr[5]);
    Serial.printf("[sec]   auth_mode=0x%02x key_present=%d key_type=%d\r\n",
                  cmpl.auth_mode, cmpl.key_present, cmpl.key_type);
    if (!cmpl.success) {
      Serial.printf("[sec]   fail_reason=0x%02x\r\n", cmpl.fail_reason);
    }
  }
};

// ---------- LED task: runs independently so blinking never stalls ----------
static void ledTask(void *) {
  bool level = false;
  for (;;) {
    switch (ledState) {
      case PAIRED:
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(200));
        break;
      case CONNECTING:
        level = !level;
        digitalWrite(LED_PIN, level);
        vTaskDelay(pdMS_TO_TICKS(50));   // 10 Hz blink
        break;
      case SCANNING:
      default:
        level = !level;
        digitalWrite(LED_PIN, level);
        vTaskDelay(pdMS_TO_TICKS(500));  // 1 Hz blink
        break;
    }
  }
}

static void startScanChunk() {
  BLEScan *scan = BLEDevice::getScan();
  scan->clearResults();
  // Blocking 3 s chunk; the callback sets foundPending when the ring shows up.
  scan->start(3, false);
}

static bool connectAndPair() {
  if (!client) {
    client = BLEDevice::createClient();
    client->setClientCallbacks(new ClientCallbacks());
  }

  ledState = CONNECTING;
  authDone = false;
  authOk = false;
  linkDown = false;

  Serial.printf("[ble] connecting to %s addr=%s\r\n", TARGET_NAME,
                target->getAddress().toString().c_str());
  uint32_t t0 = millis();
  if (!client->connect(target)) {
    Serial.printf("[ble] connect failed after %lu ms\r\n",
                  (unsigned long)(millis() - t0));
    return false;
  }
  Serial.printf("[ble] connect ok in %lu ms\r\n", (unsigned long)(millis() - t0));

  // Start encryption/pairing. If the ring is already bonded this reuses
  // the stored keys; otherwise it performs a new pairing.
  bool secOk = client->secureConnection();
  Serial.printf("[ble] secureConnection() returned %d\r\n", secOk);
  if (!secOk) {
    Serial.println("[ble] secureConnection() failed");
    client->disconnect();
    return false;
  }

  // Wait for the auth result (bounded to 30 s), logging progress every 5 s.
  uint32_t start = millis();
  uint32_t lastLog = start;
  while (!authDone && !linkDown && millis() - start < 30000) {
    delay(50);
    if (millis() - lastLog >= 5000) {
      lastLog = millis();
      Serial.printf("[ble] waiting for auth... %lu ms, connected=%d\r\n",
                    (unsigned long)(lastLog - start), client->isConnected());
    }
  }
  Serial.printf("[ble] wait ended: authDone=%d authOk=%d linkDown=%d\r\n",
                authDone, authOk, linkDown);
  if (authDone && authOk && !linkDown) {
    ledState = PAIRED;
    Serial.println("[ble] paired - LED solid");
    return true;
  }

  Serial.println("[ble] pairing did not complete");
  if (client->isConnected()) client->disconnect();
  return false;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  Serial.println();
  Serial.println("BLE ring connector starting");

  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 1);

  BLEDevice::init("ESP32-ring-led");

  BLESecurity *sec = new BLESecurity();
  sec->setAuthenticationMode(true, false, false); // bonding ON, MITM OFF, SC OFF (legacy Just Works)
  // The library only sets the encryption level when SC is on, so set it here.
  BLESecurity::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_NO_MITM);
  sec->setCapability(ESP_IO_CAP_NONE);            // Just Works
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());
  Serial.println("[sec] bonding ON, MITM OFF, SC OFF, IO cap NONE (Just Works), keys: enc+id");

  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

  ledState = SCANNING;
  Serial.printf("Scanning for \"%s\"...\r\n", TARGET_NAME);
}

void loop() {
  if (ledState == PAIRED) {
    if (linkDown) {
      // Ring dropped the link: go back to scanning.
      Serial.println("[ble] link lost, scanning again");
      linkDown = false;
      ledState = SCANNING;
    } else {
      delay(100);
      return;
    }
  }

  if (ledState == SCANNING) {
    startScanChunk();
    if (foundPending) {
      foundPending = false;
      BLEDevice::getScan()->stop();
      if (!connectAndPair()) {
        ledState = SCANNING;
        delay(1000);
      }
    }
  }
}
