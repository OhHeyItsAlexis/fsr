#include <inttypes.h>

#if !defined(__AVR_ATmega32U4__) && !defined(__AVR_ATmega328P__) && \
    !defined(__AVR_ATmega1280__) && !defined(__AVR_ATmega2560__)
  #define CAN_AVERAGE
#endif

// Default debounce for buttons.
const long kDebounceDelay = 50;
// Default threshold value for each of the sensors.
const int16_t kDefaultThreshold = 1000;
// Max window size for both of the moving averages classes.
const size_t kWindowSize = 50;
// Baud rate used for Serial communication. Technically ignored by Teensys.
const long kBaudRate = 115200;
// Max number of sensors per panel.
// NOTE(teejusb): This is arbitrary, if you need to support more sensors
// per panel then just change the following number.
const size_t kMaxSharedSensors = 2;
// Maximum number of buttons on the joystick
// NOTE(alsalkeld): This is also arbitrary. You can bump this up if you have
// additional inputs, it just make the joystick gamepad window neater if they match.
uint8_t kMaxJoystickButtons = 10;
// Automatically incremented when creating a new button or sensor.
uint8_t curButtonNum = 0;

#ifdef CORE_TEENSY
  // Use the Joystick library for Teensy
  void ButtonStart() {
    // Use Joystick.begin() for everything that's not Teensy 2.0.
    #ifndef __AVR_ATmega32U4__
      Joystick.begin();
    #endif
    Joystick.useManualSend(true);
  }
  void ButtonPress(uint8_t button_num) {
    Joystick.button(button_num, 1);
  }
  void ButtonRelease(uint8_t button_num) {
    Joystick.button(button_num, 0);
  }
#else
  #include <Joystick.h> //https://github.com/MHeironimus/ArduinoJoystickLibrary
  // And the Joystick library for Arduino
  
  // Create the Joystick
  Joystick_ Joystick(JOYSTICK_DEFAULT_REPORT_ID,JOYSTICK_TYPE_GAMEPAD,
    kMaxJoystickButtons, 0, // Button Count, Hat Switch Count
    false, false, false,   // No X, Y or Z Axis
    false, false, false,   // No Rx, Ry, or Rz
    false, false,          // No rudder or throttle
    false, false, false);  // No accelerator, brake, or steering

  void ButtonStart() {
    Joystick.begin(false);
  }
  void ButtonPress(uint8_t button_num) {
    Joystick.pressButton(button_num);
  }
  void ButtonRelease(uint8_t button_num) {
    Joystick.releaseButton(button_num);
  }
#endif

/*===========================================================================*/

// EXPERIMENTAL. Used to turn on the lights feature. Note, this might conflict
// some existing sensor pins so if you see some weird behavior it might be
// because of this. Uncomment the following line to enable the feature.

// #define ENABLE_LIGHTS

// We don't want to use digital pins 0 and 1 as they're needed for Serial
// communication so we start curLightPin from 2.
// Automatically incremented when creating a new SensorState.
#if defined(ENABLE_LIGHTS)
  uint8_t curLightPin = 2;
#endif

/*===========================================================================*/

// Calculates the Weighted Moving Average for a given period size.
// Values provided to this class should fall in [−32,768, 32,767] otherwise it
// may overflow. We use a 32-bit integer for the intermediate sums which we
// then restrict back down to 16-bits.
class WeightedMovingAverage {
 public:
  WeightedMovingAverage(size_t size) :
      size_(min(size, kWindowSize)), cur_sum_(0), cur_weighted_sum_(0),
      values_{}, cur_count_(0) {}

  int16_t GetAverage(int16_t value) {
    // Add current value and remove oldest value.
    // e.g. with value = 5 and cur_count_ = 0
    // [4, 3, 2, 1] -> 10 becomes 10 + 5 - 4 = 11 -> [5, 3, 2, 1]
    int32_t next_sum = cur_sum_ + value - values_[cur_count_];
    // Update weighted sum giving most weight to the newest value.
    // [1*4, 2*3, 3*2, 4*1] -> 20 becomes 20 + 4*5 - 10 = 30
    //     -> [4*5, 1*3, 2*2, 3*1]
    // Subtracting by cur_sum_ is the same as removing 1 from each of the weight
    // coefficients.
    int32_t next_weighted_sum = cur_weighted_sum_ + size_ * value - cur_sum_;
    cur_sum_ = next_sum;
    cur_weighted_sum_ = next_weighted_sum;
    values_[cur_count_] = value;
    cur_count_ = (cur_count_ + 1) % size_;
    // Integer division is fine here since both the numerator and denominator
    // are integers and we need to return an int anyways. Off by one isn't
    // substantial here.
    // Sum of weights = sum of all integers from [1, size_]
    return next_weighted_sum/((size_ * (size_ + 1)) / 2);
  }

  // Delete default constructor. Size MUST be explicitly specified.
  WeightedMovingAverage() = delete;

 private:
  size_t size_;
  int32_t cur_sum_;
  int32_t cur_weighted_sum_;
  // Keep track of all values we have in a circular array.
  int16_t values_[kWindowSize];
  size_t cur_count_;
};

// Calculates the Hull Moving Average. This is one of the better smoothing
// algorithms that will smooth the input values without wildly distorting the
// input values while still being responsive to input changes.
//
// The algorithm is essentially:
//   1. Calculate WMA of input values with a period of n/2 and double it.
//   2. Calculate WMA of input values with a period of n and subtract it from
//      step 1.
//   3. Calculate WMA of the values from step 2 with a period of sqrt(2).
//
// HMA = WMA( 2 * WMA(input, n/2) - WMA(input, n), sqrt(n) )
class HullMovingAverage {
 public:
  HullMovingAverage(size_t size) :
      wma1_(size/2), wma2_(size), hull_(sqrt(size)) {}

  int16_t GetAverage(int16_t value) {
    int16_t wma1_value = wma1_.GetAverage(value);
    int16_t wma2_value = wma2_.GetAverage(value);
    int16_t hull_value = hull_.GetAverage(2 * wma1_value - wma2_value);

    return hull_value;
  }

 private:
  WeightedMovingAverage wma1_;
  WeightedMovingAverage wma2_;
  WeightedMovingAverage hull_;
};

/*===========================================================================*/

// The class that actually evaluates a sensor and actually triggers the button
// press or release event. If there are multiple sensors added to a
// SensorState, they will all be evaluated first before triggering the event.
class SensorState {
 public:
  SensorState()
      : num_sensors_(0),
        #if defined(ENABLE_LIGHTS)
        kLightsPin(curLightPin++),
        #endif
        kButtonNum(curButtonNum++) {
    for (size_t i = 0; i < kMaxSharedSensors; ++i) {
      sensor_ids_[i] = 0;
      individual_states_[i] = SensorState::OFF;
    }
    #if defined(ENABLE_LIGHTS)
      pinMode(kLightsPin, OUTPUT);
    #endif
  }

  // Adds a new sensor to share this state with. If we try adding a sensor that
  // we don't have space for, it's essentially dropped.
  void AddSensor(uint8_t sensor_id) {
    if (num_sensors_ < kMaxSharedSensors) {
      sensor_ids_[num_sensors_++] = sensor_id;
    }
  }

  // Evaluates a single sensor as part of the shared state.
  void EvaluateSensor(uint8_t sensor_id,
                      int16_t cur_value,
                      int16_t user_threshold) {
    size_t sensor_index = GetIndexForSensor(sensor_id);

    // The sensor we're evaluating is not part of this shared state.
    // This should not happen.
    if (sensor_index == SIZE_MAX) {
      return;
    }

    // If we're above the threshold, turn the individual sensor on.
    if (cur_value >= user_threshold + kPaddingWidth) {
      individual_states_[sensor_index] = SensorState::ON;
    }

    // If we're below the threshold, turn the individual sensor off.
    if (cur_value < user_threshold - kPaddingWidth) {
      individual_states_[sensor_index] = SensorState::OFF;
    }
    
    // If we evaluated all the sensors this state applies to, only then
    // should we determine if we want to send a press/release event.
    bool all_evaluated = (sensor_index == num_sensors_ - 1);

    if (all_evaluated) {
      switch (combined_state_) {
        case SensorState::OFF:
          {
            // If ANY of the sensors triggered, then we trigger a button press.
            bool turn_on = false;
            for (size_t i = 0; i < num_sensors_; ++i) {
              if (individual_states_[i] == SensorState::ON) {
                turn_on = true;
                break;
              }
            }
            if (turn_on) {
              ButtonPress(kButtonNum);
              combined_state_ = SensorState::ON;
              #if defined(ENABLE_LIGHTS)
                digitalWrite(kLightsPin, HIGH);
              #endif
            }
          }
          break;
        case SensorState::ON:
          {
            // ALL of the sensors must be off to trigger a release.
            // i.e. If any of them are ON we do not release.
            bool turn_off = true;
            for (size_t i = 0; i < num_sensors_; ++i) {
              if (individual_states_[i] == SensorState::ON) {
                turn_off = false;
              }
            }
            if (turn_off) {
              ButtonRelease(kButtonNum);
              combined_state_ = SensorState::OFF;
              #if defined(ENABLE_LIGHTS)
                digitalWrite(kLightsPin, LOW);
              #endif
            }
          }
          break;
      }
    }
  }

  // Given a sensor_id, returns the index in the sensor_ids_ array.
  // Returns SIZE_MAX if not found.
  size_t GetIndexForSensor(uint8_t sensor_id) {
    for (size_t i = 0; i < num_sensors_; ++i) {
      if (sensor_ids_[i] == sensor_id) {
        return i;
      }
    }
    return SIZE_MAX;
  }

 private:
  // The collection of sensors shared with this state.
  uint8_t sensor_ids_[kMaxSharedSensors];
  // The number of sensors this state combines with.
  size_t num_sensors_;

  // Used to determine the state of each individual sensor, as well as
  // the aggregated state.
  enum State { OFF, ON };
  // The evaluated state for each individual sensor.
  State individual_states_[kMaxSharedSensors];
  // The aggregated state.
  State combined_state_ = SensorState::OFF;

  // One-tailed width size to create a window around user_threshold to
  // mitigate fluctuations by noise. 
  // TODO(teejusb): Make this a user controllable variable.
  const int16_t kPaddingWidth = 1;

  // The light pin this state corresponds to.
  #if defined(ENABLE_LIGHTS)
    const uint8_t kLightsPin;
  #endif

  // The button number this state corresponds to.
  const uint8_t kButtonNum;
};

/*===========================================================================*/

// Class containing all relevant information per sensor.
class Sensor {
 public:
  Sensor(uint8_t pin_value, String button_name, SensorState* sensor_state = nullptr)
      : initialized_(false), pin_value_(pin_value),
        button_name_(button_name),
        user_threshold_(kDefaultThreshold),
        #if defined(CAN_AVERAGE)
          moving_average_(kWindowSize),
        #endif
        offset_(0), sensor_state_(sensor_state),
        should_delete_state_(false) {}
  
  ~Sensor() {
    if (should_delete_state_) {
      delete sensor_state_;
    }
  }

  void Init(uint8_t sensor_id) {
    // Sensor should only be initialized once.
    if (initialized_) {
      return;
    }
    // Sensor IDs should be 1-indexed thus they must be non-zero.
    if (sensor_id == 0) {
      return;
    }

    // There is no state for this sensor, create one.
    if (sensor_state_ == nullptr) {
      sensor_state_ = new SensorState();
      // If this sensor created the state, then it's in charge of deleting it.
      should_delete_state_ = true;
    }

    // If this sensor hasn't been added to the state, then try adding it.
    if (sensor_state_->GetIndexForSensor(sensor_id) == SIZE_MAX) {
      sensor_state_->AddSensor(sensor_id);
    }
    sensor_id_ = sensor_id;
    initialized_ = true;
  }

  // Fetches the sensor value and maybe triggers the button press/release.
  void EvaluateSensor(bool willSend) {
    if (!initialized_) {
      return;
    }
    // If this sensor was never added to the state, then return early.
    if (sensor_state_->GetIndexForSensor(sensor_id_) == SIZE_MAX) {
      return;
    }

    int16_t sensor_value = analogRead(pin_value_);

    #if defined(CAN_AVERAGE)
      // Fetch the updated Weighted Moving Average.
      cur_value_ = moving_average_.GetAverage(sensor_value) - offset_;
    #else
      // Don't use averaging for Arduino Leonardo, Uno, Mega1280, and Mega2560
      // since averaging seems to be broken with it. This should also include
      // the Teensy 2.0 as it's the same board as the Leonardo.
      // TODO(teejusb): Figure out why and fix. Maybe due to different integer
      // widths?
      cur_value_ = sensor_value - offset_;
    #endif

    if (willSend) {
      sensor_state_->EvaluateSensor(
        sensor_id_, cur_value_, user_threshold_);
    }
  }

  void UpdateThreshold(int16_t new_threshold) {
    user_threshold_ = new_threshold;
  }

  int16_t UpdateOffset() {
    // Update the offset with the last read value. UpdateOffset should be
    // called with no applied pressure on the panels so that it will be
    // calibrated correctly.
    offset_ = cur_value_;
    return offset_;
  }

  int16_t GetCurValue() {
    return cur_value_;
  }

  int16_t GetPin() {
    return pin_value_;
  }

  String GetButtonName() {
    return button_name_;
  }

  int16_t GetThreshold() {
    return user_threshold_;
  }

  // Delete default constructor. Pin number MUST be explicitly specified.
  Sensor() = delete;
 
 private:
  // Ensures that Init() has been called at exactly once on this Sensor.
  bool initialized_;
  // The pin on the Teensy/Arduino corresponding to this sensor.
  uint8_t pin_value_;

  // The arrow this corresponds to, for debugging.
  String button_name_;

  // The user defined threshold value to activate/deactivate this sensor at.
  int16_t user_threshold_;
  
  #if defined(CAN_AVERAGE)
  // The smoothed moving average calculated to reduce some of the noise. 
  HullMovingAverage moving_average_;
  #endif

  // The latest value obtained for this sensor.
  int16_t cur_value_;
  // How much to shift the value read by during each read.
  int16_t offset_;

  // Since many sensors may refer to the same input this may be shared among
  // other sensors.
  SensorState* sensor_state_;
  // Used to indicate if the state is owned by this instance, or if it was
  // passed in from outside
  bool should_delete_state_;

  // A unique number corresponding to this sensor. Set during Init().
  uint8_t sensor_id_;
};

/*===========================================================================*/

// Class containing all relevant information per sensor.
class Button {
 public:
  Button(uint8_t pin_value, String button_name)
      : initialized_(false), pin_value_(pin_value),
        button_name_(button_name),
        debounce_delay_(kDebounceDelay),
        kButtonNum(curButtonNum++) {}

  void Init(uint8_t button_id) {
    // Sensor should only be initialized once.
    if (initialized_) {
      return;
    }
    // Sensor IDs should be 1-indexed thus they must be non-zero.
    if (button_id == 0) {
      return;
    }

    //Initalize pin for digital button presses with internal pullup resistor
    pinMode(pin_value_, INPUT_PULLUP);

    button_id_ = button_id;
    initialized_ = true;
  }

  // Fetches the sensor value and maybe triggers the button press/release.
  void EvaluateButton(bool willSend) {
    if (!initialized_) {
      return;
    }

    if ((millis() - lastDebounceTime) > debounce_delay_)
    {
      lastDebounceTime = millis();
      cur_value_ = digitalRead(pin_value_);
    }
    
    if (willSend) {
      switch (cur_value_) {
        case LOW:
          ButtonPress(kButtonNum);
          break;
        case HIGH:
          ButtonRelease(kButtonNum);
          break;
      }
    }
  }

  int16_t GetCurValue() {
    return cur_value_;
  }

  int16_t GetPin() {
    return pin_value_;
  }

  String GetButtonName() {
    return button_name_;
  }

  // Delete default constructor. Pin number MUST be explicitly specified.
  Button() = delete;
 
  private:
  // Ensures that Init() has been called at exactly once on this Sensor.
  bool initialized_;
  
  // The pin on the Teensy/Arduino corresponding to this sensor.
  uint8_t pin_value_;

  // The arrow this corresponds to, for debugging.
  String button_name_;

  // The user defined threshold value to activate/deactivate this sensor at.
  long debounce_delay_;
  
  // The last time this pin was read as high
  long lastDebounceTime = 0;

  // The latest value obtained for this sensor.
  uint8_t cur_value_;

  // A unique number corresponding to this sensor. Set during Init().
  uint8_t button_id_;

  // The button number this button corresponds to.
  const uint8_t kButtonNum;
};


/*===========================================================================*/

// Defines the sensor collections and sets the pins for them appropriately.
//
// If you want to use multiple sensors in one panel, you will want to share
// state across them. In the following example, the first and second sensors
// share state. The maximum number of sensors that can be shared for one panel
// is controlled by the kMaxSharedSensors constant at the top of this file, but
// can be modified as needed.
//
// SensorState state1;
// Sensor kSensors[] = {
//   Sensor(A0, &state1),
//   Sensor(A1, &state1),
//   Sensor(A2),
//   Sensor(A3),
//   Sensor(A4),
// };

Button kButtons[] = {
  Button(7, "Start"),
  Button(8, "Select"),
};
const size_t kNumButtons = sizeof(kButtons)/sizeof(Button);

/*===========================================================================*/

// Defines the sensor collections and sets the pins for them appropriately.
//
// If you want to use multiple sensors in one panel, you will want to share
// state across them. In the following example, the first and second sensors
// share state. The maximum number of sensors that can be shared for one panel
// is controlled by the kMaxSharedSensors constant at the top of this file, but
// can be modified as needed.
//
// SensorState state1;
// Sensor kSensors[] = {
//   Sensor(A0, &state1),
//   Sensor(A1, &state1),
//   Sensor(A2),
//   Sensor(A3),
//   Sensor(A4),
// };

  String pinMaps[19] =
  {
    "Start",
    "Select",
    "9",
    "10",
    "11",
    "12",
    "13",
    "14",
    "15",
    "16",
    "17",
    "Right",
    "Down-Right",
    "Down",
    "Down-Left",
    "Up-Left",
    "Left",
    "Up-Right",
    "Up",
  };

Sensor kSensors[] = {
  Sensor(A5, "Left"),
  Sensor(A2, "Down"),
  Sensor(A7, "Up"), 
  Sensor(A0, "Right"),
  Sensor(A4, "Up-Left"),
  Sensor(A3, "Down-Left"),
  Sensor(A6, "Up-Right"),
  Sensor(A1, "Down-Right"),
};
const size_t kNumSensors = sizeof(kSensors)/sizeof(Sensor);

/*===========================================================================*/

class SerialProcessor {
 public:
   void Init(long baud_rate) {
    Serial.begin(baud_rate);
  }

  void CheckAndMaybeProcessData() {
    while (Serial.available() > 0) {
      size_t bytes_read = Serial.readBytesUntil(
          '\n', buffer_, kBufferSize - 1);
      buffer_[bytes_read] = '\0';

      if (bytes_read == 0) { return; }
 
      switch(buffer_[0]) {
        case 'o':
        case 'O':
          UpdateOffsets();
          break;
        case 'r':
        case 'R':
          PrintReadableValues();
          break;
        case 'v':
        case 'V':
          PrintValues();
          break;
        case 't':
        case 'T':
          PrintThresholds();
        case 'h':
        case 'H':
          PrintReadableThresholds();
          break;
        default:
          UpdateAndPrintThreshold(bytes_read);
          break;
      }
    }  
  }

  void UpdateAndPrintThreshold(size_t bytes_read) {
    // Need to specify:
    // Sensor number + Threshold value.
    // {0, 1, 2, 3} + "0"-"1023"
    // e.g. 3180 (fourth FSR, change threshold to 180)
    
    if (bytes_read < 2 || bytes_read > 5) { return; }

    size_t sensor_index = buffer_[0] - '0';
    if (sensor_index < 0 || sensor_index >= kNumSensors) { return; }

    kSensors[sensor_index].UpdateThreshold(
        strtoul(buffer_ + 1, nullptr, 10));
    PrintThresholds();
  }

  void UpdateOffsets() {
    for (size_t i = 0; i < kNumSensors; ++i) {
      kSensors[i].UpdateOffset();
    }
  }

  void PrintValues() {
    Serial.print("v");
    for (size_t i = 0; i < kNumSensors; ++i) {
      Serial.print(" ");
      Serial.print(kSensors[i].GetCurValue());
    }
    Serial.print("\n");
  }

  void PrintReadableValues() {
    Serial.print("r");
    for (size_t i = 0; i < kNumSensors; ++i) {
      Serial.print(" ");
      Serial.print(kSensors[i].GetButtonName());
      Serial.print(":");
      Serial.print(kSensors[i].GetCurValue());
    }
    for (size_t i = 0; i < kNumButtons; ++i) {
      Serial.print(" ");
      Serial.print(pinMaps[kButtons[i].GetPin()-7]);
      Serial.print(":");
      Serial.print(kButtons[i].GetCurValue());
    }
    Serial.print("\n");
  }

  void PrintReadableThresholds() {
    Serial.print("h");
    for (size_t i = 0; i < kNumSensors; ++i) {
      Serial.print(" ");
      Serial.print(kSensors[i].GetButtonName());
      Serial.print(":");
      Serial.print(kSensors[i].GetThreshold());
    }
    Serial.print("\n");
  }

  void PrintThresholds() {
    Serial.print("t");
    for (size_t i = 0; i < kNumSensors; ++i) {
      Serial.print(" ");
      Serial.print(kSensors[i].GetThreshold());
    }
    Serial.print("\n");
  }

 private:
   static const size_t kBufferSize = 64;
   char buffer_[kBufferSize];
};

/*===========================================================================*/

SerialProcessor serialProcessor;
// Timestamps are always "unsigned long" regardless of board type So don't need
// to explicitly worry about the widths.
unsigned long lastSend = 0;
// loopTime is used to estimate how long it takes to run one iteration of
// loop().
long loopTime = -1;

void setup() {
  serialProcessor.Init(kBaudRate);
  ButtonStart();
  for (size_t i = 0; i < kNumSensors; ++i) {
    // Button numbers should start with 1.
    kSensors[i].Init(i + 1);
  }

  for (size_t i = 0; i < kNumButtons; ++i) {
    // Button numbers should start with 1.
    kButtons[i].Init(i + 1);
  }
}

void loop() {
  unsigned long startMicros = micros();
  // We only want to send over USB every millisecond, but we still want to
  // read the analog values as fast as we can to have the most up to date
  // values for the average.
  static bool willSend = (loopTime == -1 ||
                          startMicros - lastSend + loopTime >= 1000);

  serialProcessor.CheckAndMaybeProcessData();

  for (size_t i = 0; i < kNumSensors; ++i) {
    kSensors[i].EvaluateSensor(willSend);
  }

  for (size_t i = 0; i < kNumButtons; ++i) {
    kButtons[i].EvaluateButton(willSend);
  }

  if (willSend) {
    lastSend = startMicros;
    #ifdef CORE_TEENSY
        Joystick.send_now();
    #else
      Joystick.sendState();
    #endif
    
  }

  if (loopTime == -1) {
    loopTime = micros() - startMicros;
  }
}
