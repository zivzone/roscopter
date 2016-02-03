#include "ekf/ekf.h"

namespace ekf
{

mocapFilter::mocapFilter() :
  nh_(ros::NodeHandle()),
  nh_private_(ros::NodeHandle("~/ekf"))
{
  // retrieve params
  nh_private_.param<double>("inner_loop_rate", inner_loop_rate_, 400);
  nh_private_.param<double>("publish_rate", publish_rate_, 400);
  nh_private_.param<double>("alpha", alpha_, 0.2);
  ros_copter::importMatrixFromParamServer(nh_private_, x_hat_, "x0");
  ros_copter::importMatrixFromParamServer(nh_private_, P_, "P0");
  ros_copter::importMatrixFromParamServer(nh_private_, Q_, "Q0");
  ros_copter::importMatrixFromParamServer(nh_private_, R_IMU_, "R_IMU");
  ros_copter::importMatrixFromParamServer(nh_private_, R_Mocap_, "R_Mocap");

  // Setup publishers and subscribers
  imu_sub_ = nh_.subscribe("imu/data", 1, &mocapFilter::imuCallback, this);
  mocap_sub_ = nh_.subscribe("mocap", 1, &mocapFilter::mocapCallback, this);
  estimate_pub_ = nh_.advertise<nav_msgs::Odometry>("estimate", 1);
  is_flying_pub_ = nh_.advertise<std_msgs::Bool>("is_flying", 1);
  predict_timer_ = nh_.createTimer(ros::Duration(1.0/inner_loop_rate_), &mocapFilter::predictTimerCallback, this);
  publish_timer_ = nh_.createTimer(ros::Duration(1.0/publish_rate_), &mocapFilter::publishTimerCallback, this);

  flying_ = false;
  ROS_INFO("Done");
  return;
}

void mocapFilter::publishTimerCallback(const ros::TimerEvent &event)
{
  publishEstimate();
  return;
}

void mocapFilter::predictTimerCallback(const ros::TimerEvent &event)
{
  if(flying_){
    predictStep();
  }
  return;
}


void mocapFilter::imuCallback(const sensor_msgs::Imu msg)
{
  if(!flying_){
    if(fabs(msg.linear_acceleration.z) > 11.0){
      ROS_WARN("Now flying");
      flying_ = true;
      std_msgs::Bool flying;
      flying.data = true;
      is_flying_pub_.publish(flying);
      previous_predict_time_ = ros::Time::now();
    }
  }
  if(flying_){
    updateIMU(msg);
  }
  return;
}


void mocapFilter::mocapCallback(const geometry_msgs::TransformStamped msg)
{
  if(!flying_){
    ROS_INFO_THROTTLE(1,"Not flying, but motion capture received, estimate is copy of mocap");
    initializeX(msg);
  }else{
    updateMocap(msg);
  }
  return;
}

void mocapFilter::initializeX(geometry_msgs::TransformStamped msg)
{
  ROS_INFO_ONCE("ekf initialized");
  tf::Transform measurement;
  double roll, pitch, yaw;
  tf::transformMsgToTF(msg.transform, measurement);
  tf::Matrix3x3(measurement.getRotation()).getRPY(roll, pitch, yaw);
  x_hat_ <<
      measurement.getOrigin().getX(), // NWU to NED
      -measurement.getOrigin().getY(),
      -measurement.getOrigin().getZ(),
      0, 0, 0,
      roll, -pitch, -yaw;
  return;
}

void mocapFilter::predictStep()
{
  ros::Time now = ros::Time::now();
  double dt = (now-previous_predict_time_).toSec();
  x_hat_ = x_hat_ + dt*f(x_hat_);
  Eigen::Matrix<double, NUM_STATES, NUM_STATES> A = dfdx(x_hat_);
  P_ = P_ + dt*(A*P_ + P_*A.transpose() + Q_);
  previous_predict_time_ = now;
  return;
}

void mocapFilter::updateIMU(sensor_msgs::Imu msg)
{
  double ax, ay, az;
  double phi(x_hat_(PHI)), theta(x_hat_(THETA)), psi(x_hat_(PSI));
  double ct = cos(theta);
  double cs = cos(psi);
  double cp = cos(phi);
  double st = sin(theta);
  double ss = sin(psi);
  double sp = sin(phi);
  double tt = tan(theta);
  ax = msg.linear_acceleration.x;
  ay = msg.linear_acceleration.y;
  az = msg.linear_acceleration.z;
  static double prev_gx(msg.angular_velocity.x), prev_gy(msg.angular_velocity.y), prev_gz(msg.angular_velocity.z);
  static double filt_ax(ax), filt_ay(ay);
  p_ = LPF(prev_gx,msg.angular_velocity.x);
  q_ = LPF(prev_gy,msg.angular_velocity.y);
  r_ = LPF(prev_gz,msg.angular_velocity.z);
  filt_az_ = LPF(filt_az_, az);
  filt_ax = LPF(filt_ax, ax);
  filt_ay = LPF(filt_ay, ay);

  prev_gx = msg.angular_velocity.x;
  prev_gy = msg.angular_velocity.y;
  prev_gz = msg.angular_velocity.z;

  tf::Quaternion attitude_tf;
  tf::quaternionMsgToTF(msg.orientation, attitude_tf);
  double roll, pitch, yaw;
  tf::Matrix3x3(attitude_tf).getEulerYPR(yaw, pitch, roll);

  Eigen::Matrix<double, 3, 1> y;
//  y << filt_ax, filt_ay, filt_az_;
  y << roll, pitch, yaw;
  Eigen::Matrix<double, 3, NUM_STATES> C = Eigen::Matrix<double, 3, NUM_STATES>::Zero();
//  C <<
//  //N  E  D  U   V   W  PHI       THETA      PSI
//    0, 0, 0, 0,  0,  0, 0,        G*ct,      0,
//    0, 0, 0, 0,  0,  0, -G*ct*cp, G*st*sp,   0,
//    0, 0, 0, 0,  0,  0, G*ct*sp,  G*st*cp,   0;
  C <<
  //N  E  D  U   V   W  PHI       THETA      PSI
    0, 0, 0, 0,  0,  0, 1,        0,         0,
    0, 0, 0, 0,  0,  0, 0,        1,         0,
    0, 0, 0, 0,  0,  0, 0,        0,         1;
  Eigen::Matrix<double, NUM_STATES, 3> L;
  L.setZero();
  L = P_*C.transpose()*(R_IMU_ + C*P_*C.transpose()).inverse();
  P_ = (Eigen::MatrixXd::Identity(NUM_STATES,NUM_STATES) - L*C)*P_;
  x_hat_ = x_hat_ + L*(y - C*x_hat_);
}

void mocapFilter::updateMocap(geometry_msgs::TransformStamped msg)
{
  tf::Transform measurement;
  double roll, pitch, yaw;
  tf::transformMsgToTF(msg.transform, measurement);
  tf::Matrix3x3(measurement.getRotation()).getRPY(roll, pitch, yaw);
  Eigen::Matrix<double, 6, 1> y;
  y << measurement.getOrigin().getX(), // NWU to NED
       -measurement.getOrigin().getY(),
       -measurement.getOrigin().getZ(),
       roll, -pitch, -yaw;
  Eigen::Matrix<double, 6, NUM_STATES> C = Eigen::Matrix<double, 6, NUM_STATES>::Zero();
  C(0,PN) = 1;
  C(1,PE) = 1;
  C(2,PD) = 1;
  C(3,PHI) = 0;
  C(4,THETA) = 0;
  C(5,PSI) = 1;
  Eigen::Matrix<double, NUM_STATES, 6> L;
  L.setZero();
  L = P_*C.transpose()*(R_Mocap_ + C*P_*C.transpose()).inverse();
  P_ = (Eigen::MatrixXd::Identity(NUM_STATES,NUM_STATES) - L*C)*P_;
  x_hat_ = x_hat_ + L*(y - C*x_hat_);
}


Eigen::Matrix<double, NUM_STATES, 1> mocapFilter::f(const Eigen::Matrix<double, NUM_STATES, 1> x)
{
  double u(x(U)), v(x(V)), w(x(W));
  double phi(x(PHI)), theta(x(THETA)), psi(x(PSI));

  double ct = cos(theta);
  double cs = cos(psi);
  double cp = cos(phi);
  double st = sin(theta);
  double ss = sin(psi);
  double sp = sin(phi);
  double tt = tan(theta);

  // calculate forces - see eqs. 16-19, and 35-37.
  Eigen::Matrix<double, NUM_STATES, 1> xdot;
  xdot  << ct*cs*u + (sp*st*cs-cp*ss)*v + (cp*st*cs+sp*ss)*w,  // position (Inertial)
          cp*ss*u  + (sp*st*ss+cp*cs)*v + (cp*st*ss-sp*cs)*w,
          st*u     - sp*ct*v            - cp*ct*w,

          r_*v-q_*w - G*st,  // velocity (Body Frame)
          p_*w-r_*u + G*ct*sp,
          q_*u-p_*v + G*ct*cp + filt_az_,

          p_ + sp*tt*q_ + cp*tt*r_, // attitude (Inertial Frame)
          cp*q_    - sp*r_,
          sp/ct*q_ + cp/ct*r_;
  return xdot;
}



Eigen::Matrix<double, NUM_STATES, NUM_STATES> mocapFilter::dfdx(const Eigen::Matrix<double, NUM_STATES, 1> x)
{
  double u(x(U)), v(x(V)), w(x(W));
  double phi(x(PHI)), theta(x(THETA)), psi(x(PSI));

  double ct = cos(theta);
  double cs = cos(psi);
  double cp = cos(phi);
  double st = sin(theta);
  double ss = sin(psi);
  double sp = sin(phi);
  double tt = tan(theta);

  Eigen::Matrix<double, NUM_STATES, NUM_STATES> result;
  result <<
//PN PE PD U      V                 W
  0, 0, 0, ct*cs, (sp*st*cs-cp*ss), (cp*st*cs+sp*ss),
      0 + (cp*st*cs+sp*ss)*v + (-sp*st*cs+cp*ss)*w, // PHI
      -st*cs*u + (sp*ct*cs)*v + (cp*ct*ss)*w, // THETA
      -ct*ss*u + (-sp*st*ss-cp*cs)*v + (-cp*st*ss+sp*cs)*w, // PSI

//PN PE PD U      V                 W
  0, 0, 0, cp*ss, (sp*st*ss+cp*cs), (cp*st*ss-sp*cs),
      0 + (cp*st*ss-sp*cs)*v + (-sp*st*ss-cp*cs)*w,
      -st*ss*u + (sp*ct*ss)*v + (cp*ct*ss)*w, // THETA
      ct*cs*u  + (sp*st*cs-cp*ss)*v + (cp*st*cs+sp*ss)*w, // PSI

//PN PE PD U      V                 W
  0, 0, 0, st,  - sp*ct,           - cp*ct,
      cp*ct*v   - sp*ct*w, // PHI
      -ct*u     - sp*st*v - cp*st*w, // THETA
      0, // PSI

//PN PE PD U    V    W    PHI               THETA                       PSI
  0, 0, 0, 0,   r_,  -q_, 0,                -G*ct,                      0,
  0, 0, 0, -r_, 0,   p_,  G*ct*cp,          -G*ct*sp,                   0,
  0, 0, 0, q_,  -p_, 0,   -G*ct*sp,         -G*st*cp,                   0,
  0, 0, 0, 0,   0,   0,   cp*tt*q_-sp*tt*r_,  (sp*q_ + cp*r_)/(ct*ct),  0,
  0, 0, 0, 0,   0,   0,   -sp*q_-cp*r_,       0,                        0,
  0, 0, 0, 0,   0,   0,   (q_*cp-r_*sp)/ct,   -(q_*sp+r_*cp)*tt/ct,     0;
  return result;
}

void mocapFilter::publishEstimate()
{
  nav_msgs::Odometry estimate;
  double pn(x_hat_(PN)), pe(x_hat_(PE)), pd(x_hat_(PD));
  double u(x_hat_(U)), v(x_hat_(V)), w(x_hat_(W));
  double phi(x_hat_(PHI)), theta(x_hat_(THETA)), psi(x_hat_(PSI));

  tf::Quaternion q;
  q.setRPY(phi, theta, psi);
  geometry_msgs::Quaternion qmsg;
  tf::quaternionTFToMsg(q, qmsg);
  estimate.pose.pose.orientation = qmsg;

  estimate.pose.pose.position.x = pn;
  estimate.pose.pose.position.y = pe;
  estimate.pose.pose.position.z = pd;

  estimate.pose.covariance[0*6+0] = P_(PN, PN);
  estimate.pose.covariance[1*6+1] = P_(PE, PE);
  estimate.pose.covariance[2*6+2] = P_(PD, PD);
  estimate.pose.covariance[3*6+3] = P_(PHI, PHI);
  estimate.pose.covariance[4*6+4] = P_(THETA, THETA);
  estimate.pose.covariance[5*6+5] = P_(PSI, PSI);

  estimate.twist.twist.linear.x = u;
  estimate.twist.twist.linear.y = v;
  estimate.twist.twist.linear.z = w;
  estimate.twist.twist.angular.x = p_;
  estimate.twist.twist.angular.y = q_;
  estimate.twist.twist.angular.z = r_;

  estimate.twist.covariance[0*6+0] = P_(U, U);
  estimate.twist.covariance[1*6+1] = P_(V, V);
  estimate.twist.covariance[2*6+2] = P_(W, W);
  estimate.twist.covariance[3*6+3] = 0.05;  // not being estimated
  estimate.twist.covariance[4*6+4] = 0.05;
  estimate.twist.covariance[5*6+5] = 0.05;

  estimate.header.frame_id = "body_link";
  estimate.header.stamp = ros::Time::now();
  estimate_pub_.publish(estimate);
}


double mocapFilter::LPF(double yn, double un)
{
  return alpha_*yn+(1-alpha_)*un;
}



} // namespace ekf

