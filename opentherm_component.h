#include "esphome.h"
#include "esphome/components/sensor/sensor.h"
#include "OpenTherm.h"
#include "opentherm_switch.h"
#include "opentherm_climate.h"
#include "opentherm_binary.h"
#include "opentherm_output.h"


// Pins to OpenTherm Adapter
const int inPin = D5; 
const int outPin = D6; 

OpenTherm ot(inPin, outPin, false);

ICACHE_RAM_ATTR void handleInterrupt() {
	ot.handleInterrupt();
}

class OpenthermComponent: public PollingComponent {
private:
  const char *TAG = "opentherm_component";
  OpenthermFloatOutput *pid_output_;
public:
  Switch *thermostatSwitch = new OpenthermSwitch();
  Sensor *external_temperature_sensor = new Sensor();
  Sensor *return_temperature_sensor = new Sensor();
  Sensor *boiler_temperature = new Sensor();
  Sensor *pressure_sensor = new Sensor();
  Sensor *modulation_sensor = new Sensor();
  Sensor *heatting_target_temperature_sensor = new Sensor();
  Climate *hotWaterClimate = new OpenthermClimate();
  OpenthermClimate *heatingWaterClimate = new OpenthermClimate();
  BinarySensor *flame = new OpenthermBinarySensor();
  
  // Set 3 sec. to give time to read all sensors (and not appear in HA as not available)
  OpenthermComponent(): PollingComponent(15000) {

  }

  void set_pid_output(OpenthermFloatOutput *pid_output) { pid_output_ = pid_output; }


  void setup() override {
    // This will be called once to set up the component
    // think of it as the setup() call in Arduino
      ESP_LOGD("opentherm_component", "Setup");

      ot.begin(handleInterrupt);

      thermostatSwitch->add_on_state_callback([=](bool state) -> void {
        ESP_LOGD ("opentherm_component", "termostatSwitch_on_state_callback %d", state);    
      });

      // Adjust HeatingWaterClimate depending on PID
      heatingWaterClimate->set_supports_auto_mode(this->pid_output_ != nullptr);
      heatingWaterClimate->set_supports_two_point_target_temperature(this->pid_output_ != nullptr);
  }
  float getExternalTemperature() {
      unsigned long response = ot.sendRequest(ot.buildRequest(OpenThermRequestType::READ, OpenThermMessageID::Toutside, 0));
      return ot.isValidResponse(response) ? ot.getFloat(response) : -1;
  }

  float getReturnTemperature() {
      unsigned long response = ot.sendRequest(ot.buildRequest(OpenThermRequestType::READ, OpenThermMessageID::Tret, 0));
      return ot.isValidResponse(response) ? ot.getFloat(response) : -1;
  }
  
  float getHotWaterTemperature() {
      unsigned long response = ot.sendRequest(ot.buildRequest(OpenThermRequestType::READ, OpenThermMessageID::Tdhw, 0));
      return ot.isValidResponse(response) ? ot.getFloat(response) : -1;
  }

  bool setHotWaterTemperature(float temperature) {
	    unsigned int data = ot.temperatureToData(temperature);
      unsigned long request = ot.buildRequest(OpenThermRequestType::WRITE, OpenThermMessageID::TdhwSet, data);
      unsigned long response = ot.sendRequest(request);
      return ot.isValidResponse(response);
  }

  float getModulation() {
    unsigned long response = ot.sendRequest(ot.buildRequest(OpenThermRequestType::READ, OpenThermMessageID::RelModLevel, 0));
    return ot.isValidResponse(response) ? ot.getFloat(response) : -1;
  }

  float getPressure() {
    unsigned long response = ot.sendRequest(ot.buildRequest(OpenThermRequestType::READ, OpenThermMessageID::CHPressure, 0));
    return ot.isValidResponse(response) ? ot.getFloat(response) : -1;
  }

  void update() override {

    ESP_LOGD("opentherm_component", "update");
    
    bool enableCentralHeating = heatingWaterClimate->mode == ClimateMode::CLIMATE_MODE_HEAT;
    bool enableHotWater = hotWaterClimate->mode == ClimateMode::CLIMATE_MODE_HEAT;
    bool enableCooling = false; // this boiler is for heating only

    
    //Set/Get Boiler Status
    auto response = ot.setBoilerStatus(enableCentralHeating, enableHotWater, enableCooling);
    bool isFlameOn = ot.isFlameOn(response);
    bool isCentralHeatingActive = ot.isCentralHeatingActive(response);
    bool isHotWaterActive = ot.isHotWaterActive(response);
    float return_temperature = getReturnTemperature();
    float hotWater_temperature = getHotWaterTemperature();


    // Set temperature depending on room thermostat
    float heatting_target_temperature;
    if (this->pid_output_ != nullptr) {
      float pid_output = pid_output_->get_state();
      if (pid_output == 0.0f) {
        heatting_target_temperature = 10.0f;
      }
      else {
        heatting_target_temperature =  pid_output * (heatingWaterClimate->target_temperature_high - heatingWaterClimate->target_temperature_low) 
        + heatingWaterClimate->target_temperature_low;      
      }
      ESP_LOGD("opentherm_component", "setBoilerTemperature  at %f °C (from PID Output)", heatting_target_temperature);
    }
    else if (thermostatSwitch->state) {
      heatting_target_temperature = heatingWaterClimate->target_temperature;
      ESP_LOGD("opentherm_component", "setBoilerTemperature  at %f °C (from heating water climate)", heatting_target_temperature);
    }
    else {
      // If the room thermostat is off, set it to 10, so that the pump continues to operate
      heatting_target_temperature = 10.0;
      ESP_LOGD("opentherm_component", "setBoilerTemperature at %f °C (default low value)", heatting_target_temperature);
    }
    ot.setBoilerTemperature(heatting_target_temperature);


    // Set hot water temperature
    setHotWaterTemperature(hotWaterClimate->target_temperature);

    // Read sensor values    
    /*
    float boilerTemperature = ot.getBoilerTemperature();
    float ext_temperature = getExternalTemperature();
    float pressure = getPressure();
    float modulation = getModulation();

    // Publish sensor values
    //flame->publish_state(isFlameOn); 
    external_temperature_sensor->publish_state(ext_temperature);
    return_temperature_sensor->publish_state(return_temperature);
    boiler_temperature->publish_state(boilerTemperature);
    pressure_sensor->publish_state(pressure);
    modulation_sensor->publish_state(modulation);
    */
    heatting_target_temperature_sensor->publish_state(heatting_target_temperature);

    // Publish status of thermostat that controls hot water
    hotWaterClimate->current_temperature = hotWater_temperature;
    hotWaterClimate->action = isHotWaterActive ? ClimateAction::CLIMATE_ACTION_HEATING : ClimateAction::CLIMATE_ACTION_OFF;
    hotWaterClimate->publish_state();
    
    // Publish status of thermostat that controls heating
    heatingWaterClimate->current_temperature = return_temperature;
    heatingWaterClimate->action = isCentralHeatingActive && isFlameOn ? ClimateAction::CLIMATE_ACTION_HEATING : ClimateAction::CLIMATE_ACTION_OFF;
    heatingWaterClimate->publish_state();

  }

};