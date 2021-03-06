#include "allocator_ros.h"

#include <vector>
#include <limits>

#include <eigen_conversions/eigen_msg.h>
#include "vortex_msgs/ThrusterForces.h"
#include "uranus_dp/eigen_typedefs.h"
#include "uranus_dp/eigen_helper.h"

Allocator::Allocator(ros::NodeHandle nh) : nh(nh)
{
  sub = nh.subscribe("rov_forces", 10, &Allocator::callback, this);
  pub = nh.advertise<vortex_msgs::ThrusterForces>("thruster_forces", 10);

  if (!nh.getParam("/propulsion/dofs/num", num_dof))
    ROS_FATAL("Failed to read parameter number of dofs.");
  if (!nh.getParam("/propulsion/thrusters/num", num_thrusters))
    ROS_FATAL("Failed to read parameter number of thrusters.");
  if (!nh.getParam("/propulsion/dofs/which", dofs))
    ROS_FATAL("Failed to read parameter which dofs.");

  // Read thrust limits
  std::vector<double> thrust;
  if (!nh.getParam("/thrusters/characteristics/thrust", thrust))
  {
    min_thrust = -std::numeric_limits<double>::infinity();
    max_thrust =  std::numeric_limits<double>::infinity();
    ROS_ERROR_STREAM("Failed to read parameters min/max thrust. Defaulting to " << min_thrust << "/" << max_thrust << ".");
  }
  else
  {
    min_thrust = thrust.front();
    max_thrust = thrust.back();
  }

  // Read thrust config matrix
  Eigen::MatrixXd thrust_configuration;
  if (!getMatrixParam(nh, "propulsion/thrusters/configuration_matrix", thrust_configuration))
    ROS_FATAL("Failed to read parameter thrust config matrix.");
  Eigen::MatrixXd thrust_configuration_pseudoinverse;
  if (!pseudoinverse(thrust_configuration, thrust_configuration_pseudoinverse))
    ROS_FATAL("Failed to compute pseudoinverse of thrust config matrix.");

  Eigen::MatrixXd force_coefficient_inverse = Eigen::MatrixXd::Identity(num_thrusters, num_thrusters);

  pseudoinverse_allocator = new PseudoinverseAllocator(thrust_configuration_pseudoinverse, force_coefficient_inverse);

  ROS_INFO("Allocator: Initialized.");
}

// inline bool saturateVector(Eigen::VectorXd &v, double min, double max)
void Allocator::callback(const geometry_msgs::Wrench &msg)
{
    Eigen::VectorXd tau = rovForcesMsgToEigen(msg);

    Eigen::VectorXd u(num_thrusters);
    u = pseudoinverse_allocator->compute(tau);

    if (isFucked(u))
    {
      ROS_ERROR("Thrust vector u invalid, will not publish.");
      return;
    }

    if (!saturateVector(u, min_thrust, max_thrust))
      ROS_WARN("Thrust vector u required saturation.");

    vortex_msgs::ThrusterForces u_msg;
    thrustEigenToMsg(u, u_msg);
    u_msg.header.stamp = ros::Time::now();
    pub.publish(u_msg);
}

Eigen::VectorXd Allocator::rovForcesMsgToEigen(const geometry_msgs::Wrench &msg)
{
    // TODO: rewrite without a million ifs
    Eigen::VectorXd tau(num_dof);
    int i = 0;
    if (dofs["surge"])
    {
      tau(i) = msg.force.x;
      ++i;
    }
    if (dofs["sway"])
    {
      tau(i) = msg.force.y;
      ++i;
    }
    if (dofs["heave"])
    {
      tau(i) = msg.force.z;
      ++i;
    }
    if (dofs["roll"])
    {
      tau(i) = msg.torque.x;
      ++i;
    }
    if (dofs["pitch"])
    {
      tau(i) = msg.torque.y;
      ++i;
    }
    if (dofs["yaw"])
    {
      tau(i) = msg.torque.z;
      ++i;
    }

    if (i != num_dof)
    {
      ROS_WARN_STREAM("Allocator: Invalid length of tau vector. Is " << i << ", should be " << num_dof << ". Returning zero thrust vector.");
      return Eigen::VectorXd::Zero(num_dof);
    }

    return tau;
}
