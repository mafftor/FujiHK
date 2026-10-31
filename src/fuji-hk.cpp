#include <Arduino.h>
#include <HomeSpan.h>
#include <FujiHeatPump.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_ota_ops.h>


#define NUM_LEDS 1
#define LED_PIN 27
#define LED_BRIGHTNESS 1
#define BTN_PIN 0     // DevKit BOOT button (GPIO39 floats on a bare DevKit)

#define RX_PIN 19
#define TX_PIN 22

#define COLOR_RED       255,0,0
#define COLOR_GREEN     0,255,0
#define COLOR_GREEN_DIM 0,1,0
#define COLOR_BLUE      0,0,255
#define COLOR_MAGENTA   162,0,162
#define COLOR_YELLOW    162,162,0
#define COLOR_CYAN      0,168,168
#define COLOR_CYAN_HSV  180,100,66
#define COLOR_ORANGE    255,115,0

#define FIRMWARE_REVISION   "0.3"

#define CONFIRM_TIMEOUT_MS  4000    // how long the Home app keeps a requested value while waiting for the AC to echo it

#define WEBLOG_PORT         8080
#define WEBLOG_USER         "admin"
#define WEBLOG_ENTRIES      50

#define CRASH_BOOTS_BEFORE_ROLLBACK 3
#define STABLE_UPTIME_MS    120000  // after this long without a crash the running firmware is considered good

TaskHandle_t FujiTask; // task for handling fuji heatpump frames

typedef struct FujiStatus {
    byte isBound = 0;
    byte onOff = 0;
    byte acMode = 5;

    byte temperature = 16;
    byte fanMode = 0;

    byte economyMode = 0;
    byte swingMode = 0;
    byte swingStep = 0;
    byte controllerTemp = 16;

    byte acError = 0;

    uint32_t gen = 0;                 // pending: HomeKit write generation; shared: newest generation sent on the bus
    uint32_t unitFrames = 0;          // shared: STATUS frames received from the indoor unit
    uint32_t unitFramesAtLastWrite = 0;
} FujiStatus;

FujiStatus sharedState;
FujiStatus pendingState;
byte       pendingFields = 0;
uint32_t   writeGen = 0;              // incremented for every batch of changes made from the Home app

SemaphoreHandle_t updateMutex;
SemaphoreHandle_t pendingMutex;

enum class HeatingCoolerState {
  HeatCool=0,
  Heat=1,
  Cool=2,
};

enum class CurrentHeatingCoolerState {
  Inactive=0,
  Idle=1,
  Heating=2,
  Cooling=3,
};

char sn[32];
char acName[64];

Preferences prefs;
WebServer   webServer(WEBLOG_PORT);
bool        webServerStarted = false;
char        webLogPassword[33] = "";

// survives panic / watchdog resets, cleared on power-on
RTC_NOINIT_ATTR uint32_t bootGuardMagic;
RTC_NOINIT_ATTR uint32_t crashBoots;
#define BOOT_GUARD_MAGIC 0xF0717A11
bool firmwareMarkedGood = false;


// HomeSpan queues a HomeKit event on every setVal(), even if nothing changed
template <typename T> void setIfChanged(SpanCharacteristic *c, T v) {
  if(c->getVal<T>() != v) {
    c->setVal(v);
  }
}


// A value the Home app asked for and the AC hasn't echoed back yet. While it is active,
// loop() keeps showing the requested value instead of the (older) value seen on the bus.
struct Expectation {
  bool active = false;
  int value = 0;
  uint32_t gen = 0;
  unsigned long since = 0;
  const char *name;

  Expectation(const char *n) : name(n) {}

  void expect(int v, uint32_t g) {
    active = true;
    value = v;
    gen = g;
    since = millis();
  }

  // true while the Home app value should be kept
  bool holding(int busValue, const FujiStatus &s) {
    if(!active) {
      return false;
    }

    bool answered = s.gen >= gen && s.unitFrames > s.unitFramesAtLastWrite;

    if(answered && busValue == value) {
      active = false;
      return false;
    }

    if(millis() - since > CONFIRM_TIMEOUT_MS) {
      active = false;
      WEBLOG("AC did not confirm %s=%d (reports %d)", name, value, busValue);
      return false;
    }

    return true;
  }
};


struct FujitsuHK : Service::HeaterCooler {

    SpanCharacteristic *active;
    SpanCharacteristic *currentTemp;
    SpanCharacteristic *currentHeaterCoolerState;
    SpanCharacteristic *targetHeaterCoolerState;
    SpanCharacteristic *coolingThresholdTemperature;
    SpanCharacteristic *heatingThresholdTemperature;
    SpanCharacteristic *temperatureDisplayUnits;

    SpanCharacteristic *rotationSpeed;

    Expectation expOnOff{"onOff"};
    Expectation expMode{"mode"};
    Expectation expTemp{"temperature"};
    Expectation expFan{"fan"};

    byte lastBound = 0;
    byte lastError = 0;


    FujitsuHK() : Service::HeaterCooler(){       // constructor() method

      Serial.print("Init FujitsuHK\n");           // initialization message

      FujiStatus currentState;

      if(xSemaphoreTake(updateMutex, ( TickType_t ) 200 ) == pdTRUE) {
        memcpy(&currentState, &sharedState, sizeof(FujiStatus));
        xSemaphoreGive(updateMutex);
      } else {
        Serial.print("Failed to get MUTEX lock, using defaults\n");
      }

      active = new Characteristic::Active(currentState.isBound ? currentState.onOff == 1 : false);

      currentTemp = new Characteristic::CurrentTemperature(currentState.isBound ? currentState.controllerTemp : 22);
      currentTemp->setRange(0, 100, 1);

      currentHeaterCoolerState = new Characteristic::CurrentHeaterCoolerState(static_cast<int>(CurrentHeatingCoolerState::Inactive));

      int hkMode = convertACModeToHKMode(currentState.isBound ? currentState.acMode : 0);
      targetHeaterCoolerState = new Characteristic::TargetHeaterCoolerState(hkMode);

      coolingThresholdTemperature = new Characteristic::CoolingThresholdTemperature(currentState.isBound ? currentState.temperature : 22);
      coolingThresholdTemperature->setRange(16, 30, 1);

      heatingThresholdTemperature = new Characteristic::HeatingThresholdTemperature(currentState.isBound ? currentState.temperature : 22);
      heatingThresholdTemperature->setRange(16, 30, 1);

      temperatureDisplayUnits = new Characteristic::TemperatureDisplayUnits(0);

      rotationSpeed = new Characteristic::RotationSpeed(currentState.isBound ? currentState.fanMode*25 : 0);
      rotationSpeed->setRange(0,100,25);

    } // end constructor

    int convertACModeToHKMode(byte acMode) {

      switch(acMode) {
        case static_cast<byte>(FujiMode::COOL):
          return static_cast<int>(HeatingCoolerState::Cool);
          break;
        case static_cast<byte>(FujiMode::HEAT):
          return static_cast<int>(HeatingCoolerState::Heat);
          break;
        case static_cast<byte>(FujiMode::AUTO):
          return static_cast<int>(HeatingCoolerState::HeatCool);
      }

      Serial.printf("UNSUPPORTED MODE IN convertACModeToHKMode %d! RETURNING 0\n", acMode);
      return 0;
    }

    byte convertHKModeToACMode(int hkMode) {

      switch(hkMode) {
        case static_cast<int>(HeatingCoolerState::Cool):
          return static_cast<byte>(FujiMode::COOL);
        case static_cast<int>(HeatingCoolerState::Heat):
          return static_cast<byte>(FujiMode::HEAT);
        case static_cast<int>(HeatingCoolerState::HeatCool):
          return static_cast<byte>(FujiMode::AUTO);
      }

      Serial.printf("UNSUPPORTED MODE in convertHKModeToACMode %d! RETURNING 5\n", hkMode);
      return 5;
    }

    void clearExpectations() {
      expOnOff.active = false;
      expMode.active = false;
      expTemp.active = false;
      expFan.active = false;
    }


    void loop(){
      FujiStatus currentState;

      if(xSemaphoreTake( updateMutex, ( TickType_t ) 200 ) == pdTRUE) {
        memcpy(&currentState, &sharedState, sizeof(FujiStatus));
        xSemaphoreGive( updateMutex);
      } else {
        Serial.printf("Failed to get MUTEX lock, skipping loop\n");
        return;
      }

      if(currentState.isBound != lastBound) {
        lastBound = currentState.isBound;
        WEBLOG(lastBound ? "Connected to the AC bus" : "Lost the AC bus");
      }

      if(!currentState.isBound) {
        clearExpectations();
        return;
      }

      if(currentState.acError != lastError) {
        lastError = currentState.acError;
        WEBLOG(lastError ? "AC reports an error" : "AC error cleared");
      }

      bool holdOnOff = expOnOff.holding(currentState.onOff, currentState);
      bool holdMode  = expMode.holding(currentState.acMode, currentState);
      bool holdTemp  = expTemp.holding(currentState.temperature, currentState);
      bool holdFan   = expFan.holding(currentState.fanMode, currentState);

      if(!holdOnOff) {
        bool onOff = currentState.onOff == 1 ? true : false;

        if(active->getVal() != onOff) {
          WEBLOG("AC changed power -> %s", onOff ? "on" : "off");
          active->setVal(onOff);
        }
      }

      byte fanMode = currentState.fanMode;

      if(!holdFan && fanMode <= 4) {
        if(rotationSpeed->getVal() != fanMode*25) {
          WEBLOG("AC changed fan -> %d", fanMode);
          rotationSpeed->setVal(fanMode*25);
        }
      }

      // while a mode change is in flight, lay the thresholds out for the requested mode
      byte acMode = holdMode ? expMode.value : currentState.acMode;

      if(!holdMode) {
        byte convertedMode = convertHKModeToACMode(targetHeaterCoolerState->getVal());
        if(convertedMode != acMode) {
          switch(acMode) {
            case static_cast<byte>(FujiMode::COOL):
              targetHeaterCoolerState->setVal(static_cast<int>(HeatingCoolerState::Cool));
              break;
            case static_cast<byte>(FujiMode::HEAT):
              targetHeaterCoolerState->setVal(static_cast<int>(HeatingCoolerState::Heat));
              break;
            case static_cast<byte>(FujiMode::AUTO):
              targetHeaterCoolerState->setVal(static_cast<int>(HeatingCoolerState::HeatCool));
              break;
            case static_cast<byte>(FujiMode::FAN):
              break;
            case static_cast<byte>(FujiMode::DRY):
              break;
          }

          if(acMode == static_cast<byte>(FujiMode::HEAT) ||
             acMode == static_cast<byte>(FujiMode::COOL) ||
             acMode == static_cast<byte>(FujiMode::AUTO)) {
                 WEBLOG("AC changed mode -> %d", acMode);
          }
        }
      }

      if(currentState.controllerTemp <= 100) {
        setIfChanged(currentTemp, (float)currentState.controllerTemp);
      }

      byte temp = holdTemp ? expTemp.value : currentState.temperature;
      if(temp >= 16 && temp <= 30) {
        switch(acMode) {
          case static_cast<byte>(FujiMode::COOL):
              setIfChanged(coolingThresholdTemperature, (float)temp);
              break;

          case static_cast<byte>(FujiMode::HEAT):
              setIfChanged(heatingThresholdTemperature, (float)temp);
              break;

          case static_cast<byte>(FujiMode::AUTO):
              setIfChanged(heatingThresholdTemperature, (float)max(temp - 2, 16));
              setIfChanged(coolingThresholdTemperature, (float)min(temp + 2, 30));
              break;
        }
      }

      setIfChanged(currentHeaterCoolerState, computeCurrentState(acMode));
    }

    // HomeKit only gets "is it working right now" from us, the bus has no compressor state
    int computeCurrentState(byte acMode) {
      if(!active->getVal()) {
        return static_cast<int>(CurrentHeatingCoolerState::Inactive);
      }

      float current = currentTemp->getVal<float>();

      switch(acMode) {
        case static_cast<byte>(FujiMode::COOL):
          return static_cast<int>(current > coolingThresholdTemperature->getVal<float>() ? CurrentHeatingCoolerState::Cooling : CurrentHeatingCoolerState::Idle);

        case static_cast<byte>(FujiMode::HEAT):
          return static_cast<int>(current < heatingThresholdTemperature->getVal<float>() ? CurrentHeatingCoolerState::Heating : CurrentHeatingCoolerState::Idle);

        case static_cast<byte>(FujiMode::AUTO):
          if(current > coolingThresholdTemperature->getVal<float>()) {
            return static_cast<int>(CurrentHeatingCoolerState::Cooling);
          } else if(current < heatingThresholdTemperature->getVal<float>()) {
            return static_cast<int>(CurrentHeatingCoolerState::Heating);
          }
          return static_cast<int>(CurrentHeatingCoolerState::Idle);
      }

      return static_cast<int>(CurrentHeatingCoolerState::Idle);  // FAN / DRY
    }


    boolean update(){
      byte isBound = 0;

      if(xSemaphoreTake(updateMutex, (TickType_t)200) == pdTRUE) {
        isBound = sharedState.isBound;
        xSemaphoreGive(updateMutex);
      }

      if(!isBound) {
        // the Home app shows the write as failed instead of queueing it for whenever the bus comes back
        WEBLOG("Rejected a change from the Home app: not connected to the AC bus");
        return false;
      }

      bool inAutoMode = ( targetHeaterCoolerState->updated() && targetHeaterCoolerState->getNewVal() == static_cast<int>(HeatingCoolerState::HeatCool)) ||
                        (!targetHeaterCoolerState->updated() && targetHeaterCoolerState->getVal()    == static_cast<int>(HeatingCoolerState::HeatCool));
      bool inCoolMode = ( targetHeaterCoolerState->updated() && targetHeaterCoolerState->getNewVal() == static_cast<int>(HeatingCoolerState::Cool)) ||
                        (!targetHeaterCoolerState->updated() && targetHeaterCoolerState->getVal()    == static_cast<int>(HeatingCoolerState::Cool));
      bool inHeatMode = ( targetHeaterCoolerState->updated() && targetHeaterCoolerState->getNewVal() == static_cast<int>(HeatingCoolerState::Heat)) ||
                        (!targetHeaterCoolerState->updated() && targetHeaterCoolerState->getVal()    == static_cast<int>(HeatingCoolerState::Heat));

      // HomeKit sends floats (e.g. 22.78 when the phone is set to Fahrenheit); round instead of truncating
      int heatThreshold = lroundf(heatingThresholdTemperature->updated() ? heatingThresholdTemperature->getNewVal<float>() : heatingThresholdTemperature->getVal<float>());
      int coolThreshold = lroundf(coolingThresholdTemperature->updated() ? coolingThresholdTemperature->getNewVal<float>() : coolingThresholdTemperature->getVal<float>());
      int midPoint = (heatThreshold - coolThreshold)/2 + coolThreshold;

      byte fields = 0;
      FujiStatus changes;

      if(active->updated()) {
        if(active->getVal() != active->getNewVal()) {
          WEBLOG("Home app: power %d -> %d", active->getVal(), active->getNewVal());
          fields |= kOnOffUpdateMask;
          changes.onOff = active->getNewVal() ? 1 : 0;
        }
      }

      if(targetHeaterCoolerState->updated())
      {
        if(targetHeaterCoolerState->getVal() != targetHeaterCoolerState->getNewVal()) {
          WEBLOG("Home app: mode %d -> %d", targetHeaterCoolerState->getVal(), targetHeaterCoolerState->getNewVal());
          fields |= kModeUpdateMask;
          changes.acMode = convertHKModeToACMode(targetHeaterCoolerState->getNewVal());
        }
      }

      if(rotationSpeed->updated())
      {
        // Siri and automations may send speeds that aren't on the 25% grid
        int fanStep = constrain((int)lroundf(rotationSpeed->getNewVal<float>() / 25.0f), 0, 4);

        if(rotationSpeed->getVal() != fanStep*25) {
          WEBLOG("Home app: fan %d%% -> step %d", rotationSpeed->getVal(), fanStep);
          fields |= kFanModeUpdateMask;

          switch(fanStep){
            case 0:
              changes.fanMode = static_cast<byte>(FujiFanMode::FAN_AUTO);
              break;
            case 1:
              changes.fanMode = static_cast<byte>(FujiFanMode::FAN_QUIET);
              break;
            case 2:
              changes.fanMode = static_cast<byte>(FujiFanMode::FAN_LOW);
              break;
            case 3:
              changes.fanMode = static_cast<byte>(FujiFanMode::FAN_MEDIUM);
              break;
            case 4:
              changes.fanMode = static_cast<byte>(FujiFanMode::FAN_HIGH);
              break;
          }
        }
      }

      int newTemp = -1;

      if(coolingThresholdTemperature->updated() && inAutoMode) {
        if(coolingThresholdTemperature->getVal<float>() != coolingThresholdTemperature->getNewVal<float>()) {
          newTemp = midPoint;
        }
      } else if(coolingThresholdTemperature->updated() && inCoolMode) {
        if(coolingThresholdTemperature->getVal<float>() != coolingThresholdTemperature->getNewVal<float>()) {
          newTemp = coolThreshold;
        }
      }

      if(heatingThresholdTemperature->updated() && inAutoMode) {
        if(heatingThresholdTemperature->getVal<float>() != heatingThresholdTemperature->getNewVal<float>()) {
          newTemp = midPoint;
        }
      } else if(heatingThresholdTemperature->updated() && inHeatMode) {
        if(heatingThresholdTemperature->getVal<float>() != heatingThresholdTemperature->getNewVal<float>()) {
          newTemp = heatThreshold;
        }
      }

      if(newTemp != -1) {
        WEBLOG("Home app: temperature -> %d%s", constrain(newTemp, 16, 30), inAutoMode ? " (auto midpoint)" : "");
        fields |= kTempUpdateMask;
        changes.temperature = constrain(newTemp, 16, 30);
      }

      if(!fields) {
        return true;
      }

      if(xSemaphoreTake(pendingMutex, (TickType_t)200) != pdTRUE) {
        WEBLOG("Dropped a change from the Home app: pending queue busy");
        return false;
      }

      uint32_t gen = ++writeGen;

      if(fields & kOnOffUpdateMask)   pendingState.onOff = changes.onOff;
      if(fields & kModeUpdateMask)    pendingState.acMode = changes.acMode;
      if(fields & kFanModeUpdateMask) pendingState.fanMode = changes.fanMode;
      if(fields & kTempUpdateMask)    pendingState.temperature = changes.temperature;
      pendingState.gen = gen;
      pendingFields |= fields;

      xSemaphoreGive(pendingMutex);

      if(fields & kOnOffUpdateMask)   expOnOff.expect(changes.onOff, gen);
      if(fields & kModeUpdateMask)    expMode.expect(changes.acMode, gen);
      if(fields & kFanModeUpdateMask) expFan.expect(changes.fanMode, gen);
      if(fields & kTempUpdateMask)    expTemp.expect(changes.temperature, gen);

      return true;

    }
};


void FujiTaskLoop(void *pvParameters){
  FujiHeatPump hp;
  FujiStatus updateState;     // commands taken from the Home app
  FujiStatus busState;        // what we publish to the HomeKit side
  byte updatedFields = 0;
  uint32_t takenGen = 0;      // newest Home app generation handed to the library
  unsigned long takenAt = 0;
  bool wasBound = false;

  Serial.print("FujiTask Begin on core ");
  Serial.println(xPortGetCoreID());

  Serial.print("Creating FujiHeatPump object\n");
  hp.connect(&Serial2, true, RX_PIN, TX_PIN);

  for(;;){
      bool bound = hp.isBound();

      if(xSemaphoreTake(pendingMutex, (TickType_t)200) == pdTRUE) {
        if(pendingFields) {
          memcpy(&updateState, &pendingState, sizeof(FujiStatus));
          updatedFields = pendingFields;
          pendingFields = 0;
        }
        xSemaphoreGive(pendingMutex);
      }

      if(updatedFields && bound) {
        if(updatedFields & kOnOffUpdateMask) {
            hp.setOnOff(updateState.onOff == 1 ? true : false);
        }

        if(updatedFields & kTempUpdateMask) {
            hp.setTemp(updateState.temperature);
        }

        if(updatedFields & kModeUpdateMask) {
            hp.setMode(updateState.acMode);
        }

        if(updatedFields & kFanModeUpdateMask) {
            hp.setFanMode(updateState.fanMode);
        }

        if(updatedFields & kSwingModeUpdateMask) {
            hp.setSwingMode(updateState.swingMode);
        }

        if(updatedFields & kSwingStepUpdateMask) {
            hp.setSwingStep(updateState.swingStep);
        }

        takenGen = updateState.gen;
        takenAt = millis();
      }

      updatedFields = 0;  // changes arriving while the bus is down are dropped, update() already refuses them

      if(wasBound && !bound) {
        hp.clearPendingUpdates();   // don't replay stale commands when the bus comes back
      }

      // the Home app has already reverted to the AC's state by now, so don't apply this later
      if(hp.getUpdateFields() && millis() - takenAt > CONFIRM_TIMEOUT_MS) {
        hp.clearPendingUpdates();
      }
      wasBound = bound;

      uint32_t writesBefore = hp.getWritesSent();
      bool gotFrame = hp.waitForFrame();

      if(gotFrame) {
        delay(60);
        hp.sendPendingFrame();
      }

      bool wroteUpdates = hp.getWritesSent() != writesBefore;
      bound = hp.isBound();

      if(gotFrame || wroteUpdates || bound != busState.isBound) {
        FujiFrame *unit = hp.getUnitState();

        busState.isBound        = bound;
        busState.onOff          = unit->onOff;
        busState.acMode         = unit->acMode;
        busState.temperature    = unit->temperature;
        busState.fanMode        = unit->fanMode;
        busState.economyMode    = unit->economyMode;
        busState.swingMode      = unit->swingMode;
        busState.swingStep      = unit->swingStep;
        busState.acError        = unit->acError;
        busState.controllerTemp = hp.getCurrentState()->controllerTemp;

        if(wroteUpdates) {
          busState.gen = takenGen;
        }
        busState.unitFrames            = hp.getUnitFrames();
        busState.unitFramesAtLastWrite = hp.getUnitFramesAtLastWrite();

        if(xSemaphoreTake(updateMutex, (TickType_t)200) == pdTRUE) {
          memcpy(&sharedState, &busState, sizeof(FujiStatus)); //copy our local back to the shared
          xSemaphoreGive(updateMutex);
        }
      }

      delay(1);
  }
}


// If a firmware update keeps crashing, go back to the other OTA slot instead of boot-looping.
// HomeSpan's own safe mode only covers "new firmware has OTA disabled".
void checkBootGuard() {
  esp_reset_reason_t reason = esp_reset_reason();
  bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                 reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;

  if(bootGuardMagic != BOOT_GUARD_MAGIC || !crashed) {
    bootGuardMagic = BOOT_GUARD_MAGIC;
    crashBoots = 0;
  }

  if(crashed) {
    crashBoots++;
    Serial.printf("Boot guard: crash reset %d of %d\n", crashBoots, CRASH_BOOTS_BEFORE_ROLLBACK);
  }

  if(crashBoots >= CRASH_BOOTS_BEFORE_ROLLBACK) {
    crashBoots = 0;
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t desc;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;

    // never fall back to a slot HomeSpan's safe mode already rejected (e.g. a build without OTA)
    bool usable = other &&
                  esp_ota_get_partition_description(other, &desc) == ESP_OK &&
                  strcmp(desc.project_name, esp_ota_get_app_description()->project_name) == 0 &&
                  !(esp_ota_get_state_partition(other, &state) == ESP_OK &&
                    (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED));

    if(usable && esp_ota_set_boot_partition(other) == ESP_OK) {
      Serial.printf("Boot guard: rolling back to %s (%s)\n", other->label, desc.version);
      delay(100);
      esp_restart();
    }

    Serial.print("Boot guard: no valid firmware to roll back to\n");
  }
}

// Arduino marks a freshly OTA'd image valid before setup() unless this returns true. Keeping it
// pending means any reset before markFirmwareGood() (crash, hang + power cycle, brownout) makes
// the bootloader boot the previous slot. USB-flashed images aren't pending, so this is a no-op there.
extern "C" bool verifyRollbackLater() {
  return true;
}

void markFirmwareGood() {
  if(!firmwareMarkedGood && millis() > STABLE_UPTIME_MS && WiFi.status() == WL_CONNECTED) {
    firmwareMarkedGood = true;
    crashBoots = 0;
    esp_ota_mark_app_valid_cancel_rollback();
    WEBLOG("Firmware %s marked good", FIRMWARE_REVISION);
  }
}


void setWebLogPassword(const char *buf) {
  const char *pwd = buf + 1;      // HomeSpan passes the command letter first
  while(*pwd == ' ') {
    pwd++;
  }

  size_t len = strlen(pwd);
  if(len < 8 || len > 32) {
    Serial.print("*** Web log password must be 8-32 characters, not changed\n");
    return;
  }

  prefs.putString("weblogpw", pwd);
  strlcpy(webLogPassword, pwd, sizeof(webLogPassword));
  Serial.print("Web log password saved\n");
}

void handleWebLog() {
  if(webLogPassword[0] == '\0') {
    webServer.send(503, "text/plain", "Web log password is not set. Use the @W serial command.\n");
    return;
  }

  if(!webServer.authenticate(WEBLOG_USER, webLogPassword)) {
    webServer.requestAuthentication(DIGEST_AUTH, "FujiHK");
    return;
  }

  webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  webServer.send(200, "text/html", "");
  homeSpan.getWebLog([](const char *chunk, void *) {
    if(chunk) {
      webServer.sendContent(chunk);
    }
  }, NULL);
  webServer.sendContent("");
}

void serviceWebLog() {
  if(!webServerStarted) {
    if(WiFi.status() != WL_CONNECTED) {
      return;
    }
    webServer.on("/", handleWebLog);
    webServer.begin();
    webServerStarted = true;
    Serial.printf("Web log at http://%s:%d/\n", WiFi.localIP().toString().c_str(), WEBLOG_PORT);
  }

  webServer.handleClient();
}


void setup() {

  memset(sn, 0, 32);
  memset(acName, 0, 64);

  Serial.begin(115200);
  Serial.print("FujitsuHK startup\n");

  checkBootGuard();

  prefs.begin("fujihk", false);
  prefs.getString("weblogpw", webLogPassword, sizeof(webLogPassword));

  updateMutex = xSemaphoreCreateMutex();
  pendingMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(
                    FujiTaskLoop,   /* Task function. */
                    "FujiTask",     /* name of task. */
                    10000,          /* Stack size of task */
                    NULL,           /* parameter of the task */
                    19,             /* priority of the task */
                    &FujiTask,      /* Task handle to keep track of created task */
                    0);             /* pin task to core 0 */

  Serial.print("HomeSpan begin on core ");
  Serial.println(xPortGetCoreID());

  homeSpan.setQRID("FUJI");
  homeSpan.setControlPin(BTN_PIN);
  homeSpan.setStatusPin(2);    // DevKit: plain LED on GPIO2 (if the board has one) instead of the Atom RGB pixel
  homeSpan.setStatusAutoOff(30);
  homeSpan.setApSSID(acName);
  homeSpan.enableOTA();        // password is set with the 'O' serial command; safeLoad rejects non-HomeSpan images
  homeSpan.setSketchVersion(FIRMWARE_REVISION);
  // NULL url: keep the log, but don't serve HomeSpan's unauthenticated page; handleWebLog() serves it behind a password
  homeSpan.enableWebLog(WEBLOG_ENTRIES, "pool.ntp.org", "CET-1CEST,M3.5.0,M10.5.0/3", NULL);
  homeSpan.begin(Category::AirConditioners,"Fujitsu AirConditioner", "FUJIAC", "FUJIAC");

  new SpanUserCommand('W', "<password> - set the web log password (8-32 characters)", setWebLogPassword);

  sprintf(sn, "%08X", (uint32_t)ESP.getEfuseMac());
  sprintf(acName, "Fujitsu AirConditioner %s", sn);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Name(acName);
      new Characteristic::Manufacturer("Fujitsu");
      new Characteristic::SerialNumber(sn);
      new Characteristic::Model("Fuji-HK");
      new Characteristic::FirmwareRevision(FIRMWARE_REVISION);
      new Characteristic::Identify();

    new Service::HAPProtocolInformation();
      new Characteristic::Version("1.1.0");

    new FujitsuHK();

  WEBLOG("Booted firmware %s from %s, reset reason %d", FIRMWARE_REVISION, esp_ota_get_running_partition()->label, esp_reset_reason());

  Serial.print("Finished setup\n");
}

void loop() {
  homeSpan.poll();
  serviceWebLog();
  markFirmwareGood();
}
