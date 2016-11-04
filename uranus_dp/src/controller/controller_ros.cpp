#include "controller_ros.h"

#include "uranus_dp/eigen_helper.h"
#include <tf/transform_datatypes.h>
#include <eigen_conversions/eigen_msg.h>

Controller::Controller(ros::NodeHandle nh) : nh(nh)
{
  command_sub = nh.subscribe("propulsion_command", 10, &Controller::commandCallback, this);
  state_sub   = nh.subscribe("state_estimate", 10, &Controller::stateCallback, this);
  wrench_pub  = nh.advertise<geometry_msgs::Wrench>("rov_forces", 10);

  control_mode = ControlModes::OPEN_LOOP;
  prev_time_valid = false;

  position_state.setZero();
  orientation_state.setIdentity();
  velocity_state.setZero();
  position_setpoint.setZero();
  orientation_setpoint.setIdentity();
  wrench_setpoint.setZero();

  getParams();

  // Read controller gains from parameter server
  std::map<std::string,double> gains;
  if (!nh.getParam("/controller/gains", gains))
    ROS_ERROR("Failed to read parameter controller gains.");

  // Read center of gravity and buoyancy vectors
  std::vector<double> r_G_vec, r_B_vec;
  if (!nh.getParam("/physical/center_of_mass", r_G_vec))
    ROS_ERROR("Failed to read robot center of mass parameter.");
  if (!nh.getParam("/physical/center_of_buoyancy", r_B_vec))
    ROS_ERROR("Failed to read robot center of buoyancy parameter.");
  Eigen::Vector3d r_G(r_G_vec.data());
  Eigen::Vector3d r_B(r_B_vec.data());

  // Read and calculate ROV weight and buoyancy
  double mass, displacement, acceleration_of_gravity, density_of_water;
  if (!nh.getParam("/physical/mass_kg", mass))
    ROS_ERROR("Failed to read parameter mass.");
  if (!nh.getParam("/physical/displacement_m3", displacement))
    ROS_ERROR("Failed to read parameter displacement.");
  if (!nh.getParam("/gravity/acceleration", acceleration_of_gravity))
    ROS_ERROR("Failed to read parameter acceleration of gravity");
  if (!nh.getParam("/water/density", density_of_water))
    ROS_ERROR("Failed to read parameter density of water");
  double W = mass * acceleration_of_gravity;
  double B = density_of_water * displacement * acceleration_of_gravity;

  position_hold_controller = new QuaternionPdController(gains["a"], gains["b"], gains["c"], W, B, r_G, r_B);

  // Set up a dynamic reconfigure server
  dynamic_reconfigure::Server<uranus_dp::ControllerConfig>::CallbackType cb;
  cb = boost::bind(&Controller::configCallback, this, _1, _2);
  dr_srv.setCallback(cb);

  ROS_INFO("Controller: Initialized with dynamic reconfigure.");
}

void Controller::commandCallback(const vortex_msgs::PropulsionCommand& msg)
{
  if (!healthyMessage(msg))
  {
    ROS_WARN("Controller: Propulsion command message out of range, ignoring...");
    return;
  }

  ControlMode new_control_mode;
  {
    int i;
    for (i = 0; i < msg.control_mode.size(); ++i)
      if (msg.control_mode[i])
        break;
    new_control_mode = static_cast<ControlMode>(i);
  }

  if (new_control_mode != control_mode)
  {
    control_mode = new_control_mode;
    switch (control_mode)
    {
      case ControlModes::OPEN_LOOP:
      ROS_INFO("Controller: Changing mode to OPEN LOOP.");
      break;

      case ControlModes::POSITION_HOLD:
      ROS_INFO("Controller: Changing mode to POSITION HOLD.");
      position_setpoint    = position_state;
      orientation_setpoint = orientation_state;
      prev_time_valid = false;
      break;
    }
  }

  updateSetpoints(msg);
}

void Controller::stateCallback(const nav_msgs::Odometry &msg)
{
  tf::pointMsgToEigen(msg.pose.pose.position, position_state);
  tf::quaternionMsgToEigen(msg.pose.pose.orientation, orientation_state);
}

void Controller::configCallback(uranus_dp::ControllerConfig &config, uint32_t level)
{
  ROS_INFO_STREAM("Setting quat pd gains:   a = " << config.a << ",   b = " << config.b << ",   c = " << config.c);
  position_hold_controller->setGains(config.a, config.b, config.c);
}

void Controller::spin()
{
  ros::Rate rate(frequency);
  while (ros::ok())
  {
    Eigen::Vector6d tau;
    switch (control_mode)
    {
      case ControlModes::POSITION_HOLD:
      tau = position_hold_controller->compute(position_state,
                                              orientation_state,
                                              velocity_state,
                                              position_setpoint,
                                              orientation_setpoint);
      break;

      case ControlModes::OPEN_LOOP:
      tau = wrench_setpoint;
      break;
    }

    geometry_msgs::Wrench msg;
    tf::wrenchEigenToMsg(tau, msg);
    wrench_pub.publish(msg);

    ros::spinOnce();
    rate.sleep();
  }
}

void Controller::updateSetpoints(const vortex_msgs::PropulsionCommand& msg)
{
  // Update wrench setpoints
  for (int i = 0; i < 6; ++i) // TODO: Fix magic number
    wrench_setpoint(i) = wrench_command_scaling[i] * wrench_command_max[i] * msg.motion[i];

  // Update pose setpoints
  if (!prev_time_valid)
  {
    prev_time = msg.header.stamp;
    prev_time_valid = true;
    return;
  }

  // Calculate time difference
  ros::Time curr_time = msg.header.stamp;
  double dt = (curr_time - prev_time).toSec();
  prev_time = curr_time;

  if (dt == 0)
  {
    // ROS_WARN("Zero time difference between propulsion command messages.");
    return;
  }

  // Increment position setpoints
  for (int i = 0; i < 3; ++i) // TODO: Fix magic number
    position_setpoint(i) += pose_command_rate[i] * dt * msg.motion[i];

  // Calc euler setpoints
  Eigen::Vector3d orientation_setpoint_euler;
  orientation_setpoint_euler = orientation_setpoint.toRotationMatrix().eulerAngles(0,1,2);
  // Increment euler setpoints
  for (int i = 0; i < 3; ++i) // TODO: Fix magic number
    orientation_setpoint_euler(i) += pose_command_rate[3+i] * dt * msg.motion[3+i];
  // Calc incremented quat setpoints
  Eigen::Matrix3d R;
  R = Eigen::AngleAxisd(orientation_setpoint_euler(0), Eigen::Vector3d::UnitX())
    * Eigen::AngleAxisd(orientation_setpoint_euler(1), Eigen::Vector3d::UnitY())
    * Eigen::AngleAxisd(orientation_setpoint_euler(2), Eigen::Vector3d::UnitZ());
  Eigen::Quaterniond q(R);
  orientation_setpoint = q;
}

void Controller::getParams()
{
  // Read control command parameters
  if (!nh.getParam("/propulsion/command/wrench/max", wrench_command_max))
    ROS_FATAL("Failed to read parameter max wrench command.");
  if (!nh.getParam("/propulsion/command/wrench/scaling", wrench_command_scaling))
    ROS_FATAL("Failed to read parameter scaling wrench command.");
  if (!nh.getParam("/propulsion/command/pose/rate", pose_command_rate))
    ROS_FATAL("Failed to read parameter pose command rate.");

  if (!nh.getParam("/controller/frequency", frequency))
  {
    ROS_WARN("Failed to read parameter controller frequency, defaulting to 10 Hz.");
    frequency = 10;
  }
}

bool Controller::healthyMessage(const vortex_msgs::PropulsionCommand& msg)
{
  // Check that motion commands are in range
  for (int i = 0; i < msg.motion.size(); ++i)
  {
    if (msg.motion[i] > 1 || msg.motion[i] < -1)
    {
      ROS_WARN("Controller: Motion command out of range.");
      return false;
    }
  }

  // Check that exactly one control mode is requested
  int num_requested_modes = 0;
  for (int i = 0; i < msg.control_mode.size(); ++i)
    if (msg.control_mode[i])
      num_requested_modes++;
  if (num_requested_modes != 1)
  {
    ROS_WARN("controller: Invalid control mode.");
    return false;
  }

  return true;
}
