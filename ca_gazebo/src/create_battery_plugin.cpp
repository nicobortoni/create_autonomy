/*
 *  Gazebo - Outdoor Multi-Robot Simulator
 *  Copyright (C) 2021
 *     Emiliano Borghi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */
/*
 * Desc: Battery plugin
 * Author: Emiliano Borghi
 * Date: 22 Jan 2021
 */

#include <string>
#include <vector>

#include <ca_gazebo/create_battery_plugin.h>

namespace gazebo
{
// Constructor
GazeboRosBattery::GazeboRosBattery()
{
}

// Destructor
GazeboRosBattery::~GazeboRosBattery()
{
  finiChild();
}

// Load the controller
void GazeboRosBattery::Load(physics::ModelPtr parent, sdf::ElementPtr sdf)
{
  parent_ = parent;

  gazebo_ros_ = GazeboRosPtr(new GazeboRos(parent, sdf, "Battery"));
  gazebo_ros_->isInitialized();

  // topic params
  gazebo_ros_->getParameter<int>(num_of_consumers_, "num_of_consumers", 2);

  gazebo_ros_->getParameter<std::string>(consumer_topic_, "consumer_topic", "/battery/consumer");
  gazebo_ros_->getParameter<std::string>(frame_id_, "frame_id", "battery");
  gazebo_ros_->getParameter<std::string>(battery_topic_, "battery_topic", "/battery/status");

  gazebo_ros_->getParameter<bool>(publish_voltage_, "publish_voltage", true);
  if (publish_voltage_)
  {
    gazebo_ros_->getParameter<std::string>(battery_voltage_topic_, "battery_voltage_topic", "/battery/voltage");
  }

  // common parameters
  gazebo_ros_->getParameter<int>(technology_, "technology", sensor_msgs::BatteryState::POWER_SUPPLY_TECHNOLOGY_LION);
  gazebo_ros_->getParameter<int>(cell_count_, "number_of_cells", 8);
  gazebo_ros_->getParameter<double>(update_rate_, "update_rate", 10.0);
  gazebo_ros_->getParameter<double>(design_capacity_, "design_capacity", 4.0);
  gazebo_ros_->getParameter<double>(nominal_voltage_, "nominal_voltage", 24.0);
  gazebo_ros_->getParameter<double>(cut_off_voltage_, "cut_off_voltage", 18.0);
  gazebo_ros_->getParameter<double>(constant_voltage_, "full_charge_voltage", 24.2);
  gazebo_ros_->getParameter<double>(lpf_tau_, "current_filter_tau", 1.0);
  gazebo_ros_->getParameter<double>(temp_lpf_tau_, "temperature_response_tau", 0.5);
  gazebo_ros_->getParameter<double>(internal_resistance_, "internal_resistance", 0.05);

  // model specific params
  gazebo_ros_->getParameter<bool>(use_nonlinear_model_, "use_nonlinear_model", true);
  if (!use_nonlinear_model_)
  {
    gazebo_ros_->getParameter<double>(lin_discharge_coeff_, "linear_discharge_coeff", -1.0);
  }
  else
  {
    gazebo_ros_->getParameter<double>(polarization_constant_, "polarization_constant", 0.07);
    gazebo_ros_->getParameter<double>(exponential_voltage_, "exponential_voltage", 0.7);
    gazebo_ros_->getParameter<double>(exponential_capacity_, "exponential_capacity", 3.0);
    gazebo_ros_->getParameter<double>(characteristic_time_, "characteristic_time", 200.0);
    gazebo_ros_->getParameter<double>(design_temperature_, "design_temperature", 25.0);
    gazebo_ros_->getParameter<double>(arrhenius_rate_polarization_, "arrhenius_rate_polarization", 500.0);
    gazebo_ros_->getParameter<double>(reversible_voltage_temp_, "reversible_voltage_temp", 0.05);
    gazebo_ros_->getParameter<double>(capacity_temp_coeff_, "capacity_temp_coeff", 0.01);
  }

  battery_state_.header.frame_id = frame_id_;
  battery_state_.power_supply_technology = technology_;
  battery_state_.power_supply_health = sensor_msgs::BatteryState::POWER_SUPPLY_HEALTH_GOOD;
  battery_state_.power_supply_status = sensor_msgs::BatteryState::POWER_SUPPLY_STATUS_UNKNOWN;
  battery_state_.design_capacity = design_capacity_;
  battery_state_.capacity = design_capacity_;
  battery_state_.present = true;

  // start
  if (update_rate_ > 0.0)
    update_period_ = 1.0 / update_rate_;
  else
    update_period_ = 0.0;
  last_update_time_ = parent_->GetWorld()->SimTime();

  // subscribers
  std::vector<ros::SubscribeOptions> subscribe_options;
  ROS_INFO_NAMED(plugin_name_, "%s: Creating %d consumer subscribers:", gazebo_ros_->info(), num_of_consumers_);
  for (int i = 0; i < num_of_consumers_; i++)
  {
    subscribe_options.push_back(ros::SubscribeOptions::create<std_msgs::Float32>(
        consumer_topic_ + "/" + std::to_string(i), 1,
        boost::bind(&GazeboRosBattery::currentCallback, this, _1, i), ros::VoidPtr(), &queue_));
    ROS_INFO_NAMED(plugin_name_, "%s: Listening to consumer on: %s", gazebo_ros_->info(),
                   (consumer_topic_ + "/" + std::to_string(i)).c_str());
    current_draw_subscribers_.push_back(gazebo_ros_->node()->subscribe(subscribe_options[i]));
    drawn_currents_.push_back(0);
  }

  // publishers
  battery_state_publisher_ = gazebo_ros_->node()->advertise<sensor_msgs::BatteryState>(battery_topic_, 1);
  ROS_INFO_NAMED(plugin_name_, "%s: Advertising battery state on %s ", gazebo_ros_->info(), battery_topic_.c_str());
  if (publish_voltage_)
  {
    battery_voltage_publisher_ = gazebo_ros_->node()->advertise<std_msgs::Float32>(battery_voltage_topic_, 1);
    ROS_INFO_NAMED(plugin_name_, "%s: Advertising battery voltage on %s ", gazebo_ros_->info(),
                   battery_voltage_topic_.c_str());
  }

  // start custom queue
  callback_queue_thread_ = std::thread(std::bind(&GazeboRosBattery::queueThread, this));

  // listen to the update event (broadcast every simulation iteration)
  update_connection_ =
      event::Events::ConnectWorldUpdateBegin(std::bind(&GazeboRosBattery::updateChild, this));

  // services
  set_charge_state =
      gazebo_ros_->node()->advertiseService(plugin_name_ + "/set_charge", &GazeboRosBattery::setCharge, this);
  set_temperature = gazebo_ros_->node()->advertiseService(plugin_name_ + "/set_temperature",
                                                                &GazeboRosBattery::setTemperature, this);
  reset_model =
      gazebo_ros_->node()->advertiseService(plugin_name_ + "/reset", &GazeboRosBattery::resetModel, this);

  model_initialised_ = false;
  Reset();
}

bool GazeboRosBattery::setTemperature(ca_gazebo::SetTemperature::Request& req,  // NOLINT(runtime/references)
                                      ca_gazebo::SetTemperature::Response& res)  // NOLINT(runtime/references)
{
  service_lock.lock();
  temp_set_ = req.temperature.data;
  ROS_WARN_NAMED(plugin_name_, "%s: Temperature set: %f", gazebo_ros_->info(), temp_set_);
  res.temperature.data = temp_set_;
  service_lock.unlock();
  return true;
}

bool GazeboRosBattery::setCharge(ca_gazebo::SetCharge::Request& req,  // NOLINT(runtime/references)
                                 ca_gazebo::SetCharge::Response& res)  // NOLINT(runtime/references)
{
  service_lock.lock();
  discharge_ = req.charge.data;
  ROS_WARN_NAMED(plugin_name_, "%s: Charge set: %f", gazebo_ros_->info(), design_capacity_ + discharge_);
  res.set.data = true;
  service_lock.unlock();
  return true;
}

bool GazeboRosBattery::resetModel(ca_gazebo::Reset::Request& req,  // NOLINT(runtime/references)
                                  ca_gazebo::Reset::Response& res)  // NOLINT(runtime/references)
{
  service_lock.lock();
  if (req.reset.data)
    Reset();
  res.state.data = req.reset.data;
  service_lock.unlock();
  return true;
}

void GazeboRosBattery::Reset()
{
  last_update_time_ = parent_->GetWorld()->SimTime();
  charge_ = design_capacity_;
  voltage_ = constant_voltage_;
  temperature_ = design_temperature_;
  temp_set_ = design_temperature_;
  discharge_ = 0;
  current_drawn_ = 0;
  battery_empty_ = false;
  internal_cutt_off_ = false;
  model_initialised_ = true;
  ROS_INFO_NAMED(plugin_name_, "%s: Battery model reset.", gazebo_ros_->info());
}

// simple linear discharge model
void GazeboRosBattery::linearDischargeVoltageUpdate()
{
  voltage_ = constant_voltage_ + lin_discharge_coeff_ * (1 - discharge_ / design_capacity_) -
             internal_resistance_ * current_lpf_;
}

// Temperature dependent parameters:
//    qt = Q + dQdT*(t-t0)
//    kt = K * np.exp(Kalpha*(1/t-1/t0))
//    e0_t = E0 + Tshift*(t-t0)
// v(i) = e0_t - kt * qt / (qt - it) * (i * Ctime / 3600.0 + it) + A * np.exp(-B * it)
//
//  where
//  E0: constant voltage [V]
//  K:  polarization constant [V/Ah] or pol. resistance [Ohm]
//  Q:  maximum capacity [Ah]
//  A:  exponential voltage [V]
//  B:  exponential capacity [A/h]
//  it: already extracted capacity [Ah] (= - discharge)
//  id: current * characteristic time [Ah]
//  i:  battery current [A]
//  T0: design temperature [Celsius]
// Tpol: name of the Vulkan first officer aboard Enterprise NX-01 [Celsius]
// Tshift: temperature offset [Celsius]

void GazeboRosBattery::nonlinearDischargeVoltageUpdate()
{
  double t = temperature_ + 273.15;
  double t0 = design_temperature_ + 273.15;
  double e0_t = constant_voltage_ + reversible_voltage_temp_ * (t - t0);  // TODO(eborghi10): Don't increase for t>t0 ?
  double qt = design_capacity_ + capacity_temp_coeff_ * (t - t0);         // TODO(eborghi10): Don't increase for t>t0 ?
  double kt = polarization_constant_ * exp(arrhenius_rate_polarization_ * (1 / t - 1 / t0));
  voltage_ = e0_t - kt * qt / (qt + discharge_) * (current_lpf_ * (characteristic_time_ / 3600.0) - discharge_) +
             exponential_voltage_ * exp(-exponential_capacity_ * -discharge_);
}

// Plugin update function
void GazeboRosBattery::updateChild()
{
  common::Time current_time = parent_->GetWorld()->SimTime();
  double dt = (current_time - last_update_time_).Double();

  if (dt > update_period_ && model_initialised_)
  {
    double n = dt / temp_lpf_tau_;
    temperature_ = temperature_ + n * (temp_set_ - temperature_);

    if (!battery_empty_)
    {
      // LPF filter on current
      double k = dt / lpf_tau_;
      current_lpf_ = current_lpf_ + k * (current_drawn_ - current_lpf_);
      // Accumulate discharge (negative: 0 to design capacity)
      discharge_ = discharge_ - GZ_SEC_TO_HOUR(dt * current_lpf_);
      if (!internal_cutt_off_)
      {
        if (use_nonlinear_model_)
        {
          nonlinearDischargeVoltageUpdate();
        }
        else
        {
          linearDischargeVoltageUpdate();
        }
        charge_ = design_capacity_ + discharge_;  // discharge is negative
        charge_memory_ = charge_;
      }
      if (voltage_ <= cut_off_voltage_ && !internal_cutt_off_)
      {
        discharge_ = 0;
        voltage_ = 0;
        internal_cutt_off_ = true;
        charge_ = charge_memory_;
        ROS_WARN_NAMED(plugin_name_, "%s: Battery voltage cut off.", gazebo_ros_->info());
      }
    }

    if (!battery_empty_ && charge_ <= 0)
    {
      discharge_ = 0;
      current_lpf_ = 0;
      voltage_ = 0;
      battery_empty_ = true;
      ROS_WARN_NAMED(plugin_name_, "%s: Battery discharged.", gazebo_ros_->info());
    }
    // state update
    battery_state_.header.stamp = ros::Time::now();
    battery_state_.voltage = voltage_;
    battery_state_.current = current_lpf_;
    battery_state_.charge = charge_;
    battery_state_.percentage = (charge_ / design_capacity_) * 100;
    battery_state_publisher_.publish(battery_state_);
    std_msgs::Float32 battery_voltage;
    battery_voltage.data = voltage_;
    if (publish_voltage_)
      battery_voltage_publisher_.publish(battery_voltage);
    last_update_time_ += common::Time(update_period_);
  }
}

// Finalize the controller
void GazeboRosBattery::finiChild()
{
  gazebo_ros_->node()->shutdown();
}

void GazeboRosBattery::currentCallback(const std_msgs::Float32::ConstPtr& current, int consumer_id)
{
  current_drawn_ = 0;
  drawn_currents_[consumer_id] = current->data;
  for (int i = 0; i < num_of_consumers_; i++)
    current_drawn_ += drawn_currents_[i];

  // temporary solution for simple charging
  if (current_drawn_ <= 0)
  {
    battery_state_.power_supply_status = sensor_msgs::BatteryState::POWER_SUPPLY_STATUS_CHARGING;
    if (internal_cutt_off_)
      voltage_ = cut_off_voltage_ + 0.05;
    internal_cutt_off_ = false;
  }
  else
  {
    battery_state_.power_supply_status = sensor_msgs::BatteryState::POWER_SUPPLY_STATUS_DISCHARGING;
  }
}

void GazeboRosBattery::queueThread()
{
  static const double TIMEOUT = 0.01;
  while (gazebo_ros_->node()->ok())
  {
    queue_.callAvailable(ros::WallDuration(TIMEOUT));
  }
}

GZ_REGISTER_MODEL_PLUGIN(GazeboRosBattery)
}  // namespace gazebo
