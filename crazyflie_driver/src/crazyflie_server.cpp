#include "ros/ros.h"
#include "crazyflie_driver/AddCrazyflie.h"
#include "crazyflie_driver/LogBlock.h"
#include "crazyflie_driver/GenericLogData.h"
#include "crazyflie_driver/UpdateParams.h"
#include "std_srvs/Empty.h"
#include "geometry_msgs/Twist.h"
#include "nav_msgs/Odometry.h" //THIAGO
#include "sensor_msgs/Imu.h"
#include "sensor_msgs/Temperature.h"
#include "sensor_msgs/MagneticField.h"
#include "std_msgs/Float32.h"

//#include <regex>
#include <thread>
#include <mutex>

#include <crazyflie_cpp/Crazyflie.h>

constexpr double pi() { return std::atan(1)*4; }

double degToRad(double deg) {
    return deg / 180.0 * pi();
}

double radToDeg(double rad) {
    return rad * 180.0 / pi();
}

class CrazyflieROS
{
public:
  CrazyflieROS(
    const std::string& link_uri,
    const std::string& tf_prefix,
    float roll_trim,
    float pitch_trim,
    bool enable_logging,
    bool enable_parameters,
    std::vector<crazyflie_driver::LogBlock>& log_blocks)
    : m_cf(link_uri)
    , m_tf_prefix(tf_prefix)
    , m_isEmergency(false)
    , m_roll_trim(roll_trim)
    , m_pitch_trim(pitch_trim)
    , m_enableLogging(enable_logging)
    , m_enableParameters(enable_parameters)
    , m_logBlocks(log_blocks)
    , m_serviceEmergency()
    , m_serviceUpdateParams()
    , m_subscribeCmdVel()
//	, m_subscribeMocapXYZ()  //THIAGO
    , m_pubImu()
    , m_pubTemp()
    , m_pubMag()
    , m_pubPressure()
    , m_pubBattery()
    , m_sentSetpoint(false)
  {
    ros::NodeHandle n;
   m_subscribeCmdVel = n.subscribe(tf_prefix + "/cmd_vel", 1, &CrazyflieROS::cmdVelChanged, this); 
	//THIAGO MOD
//	m_subscribeMocapXYZ = n.subscribe(tf_prefix + "/mocap_xyz", 1, &CrazyflieROS::cmdVelChanged, this); //Mod here!
//	m_subscribePathReference = n.subscribe(tf_prefix + "/path_ref", 1, &CrazyflieROS::cmdVelChanged, this); //Mod here!
	// END THIAGO MOD
    m_serviceEmergency = n.advertiseService(tf_prefix + "/emergency", &CrazyflieROS::emergency, this);
    m_serviceUpdateParams = n.advertiseService(tf_prefix + "/update_params", &CrazyflieROS::updateParams, this);

    m_pubImu = n.advertise<sensor_msgs::Imu>(tf_prefix + "/imu", 10);
    m_pubTemp = n.advertise<sensor_msgs::Temperature>(tf_prefix + "/temperature", 10);
    m_pubMag = n.advertise<sensor_msgs::MagneticField>(tf_prefix + "/magnetic_field", 10);
    m_pubPressure = n.advertise<std_msgs::Float32>(tf_prefix + "/pressure", 10);
    m_pubBattery = n.advertise<std_msgs::Float32>(tf_prefix + "/battery", 10);

    for (auto& logBlock : m_logBlocks)
    {
      m_pubLogDataGeneric.push_back(n.advertise<crazyflie_driver::GenericLogData>(tf_prefix + "/" + logBlock.topic_name, 10));
    }

    std::thread t(&CrazyflieROS::run, this);
    t.detach();
  }

private:
  struct logImu {
    float acc_x;
    float acc_y;
    float acc_z;
    float gyro_x;
    float gyro_y;
    float gyro_z;
  } __attribute__((packed));

  struct log2 {
    float mag_x;
    float mag_y;
    float mag_z;
    float baro_temp;
    float baro_pressure;
    float pm_vbat;
  } __attribute__((packed));

private:
  bool emergency(
    std_srvs::Empty::Request& req,
    std_srvs::Empty::Response& res)
  {
    ROS_FATAL("Emergency requested!");
    m_isEmergency = true;

    return true;
  }

  template<class T, class U>
  void updateParam(uint8_t id, const std::string& ros_param) {
      U value;
      ros::param::get(ros_param, value);
      m_cf.setParam<T>(id, (T)value);
  }

  bool updateParams(
    crazyflie_driver::UpdateParams::Request& req,
    crazyflie_driver::UpdateParams::Response& res)
  {
    ROS_INFO("Update parameters");
    for (auto&& p : req.params) {
      std::string ros_param = "/" + m_tf_prefix + "/" + p;
      size_t pos = p.find("/");
      std::string group(p.begin(), p.begin() + pos);
      std::string name(p.begin() + pos + 1, p.end());

      auto entry = m_cf.getParamTocEntry(group, name);
      if (entry)
      {
        switch (entry->type) {
          case Crazyflie::ParamTypeUint8:
            updateParam<uint8_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeInt8:
            updateParam<int8_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeUint16:
            updateParam<uint16_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeInt16:
            updateParam<int16_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeUint32:
            updateParam<uint32_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeInt32:
            updateParam<int32_t, int>(entry->id, ros_param);
            break;
          case Crazyflie::ParamTypeFloat:
            updateParam<float, float>(entry->id, ros_param);
            break;
        }
      }
      else {
        ROS_ERROR("Could not find param %s/%s", group.c_str(), name.c_str());
      }
    }
    return true;
  }

  void cmdVelChanged(
    const nav_msgs::Odometry::ConstPtr& msg)
  {
    if (!m_isEmergency) {
	  ///USE VICON POSN
      float roll = msg->twist.twist.linear.y + m_roll_trim;
      float pitch = - (msg->twist.twist.linear.x + m_pitch_trim);
      float yawrate = msg->twist.twist.angular.z;
      uint16_t thrust = std::min<uint16_t>(std::max<float>(msg->twist.twist.linear.z, 0.0), 60000);
	  //THIAGO MOD : Desired Position
	  float xd = msg->pose.pose.position.x;
	  float yd = msg->pose.pose.position.y;
	  float zd = msg->pose.pose.position.z;
	  float yawmocap = msg->pose.pose.orientation.w;
	  //THIAGO MOD: Desired Pose
	  // 

      m_cf.sendSetpoint(roll, pitch, yawrate, thrust,xd,yd,zd, yawmocap);
      m_sentSetpoint = true;
    }
  }

  void run()
  {
    // m_cf.reboot();

    auto start = std::chrono::system_clock::now();

    if (m_enableParameters)
    {
      ROS_INFO("Requesting parameters...");
      m_cf.requestParamToc();
      for (auto iter = m_cf.paramsBegin(); iter != m_cf.paramsEnd(); ++iter) {
        auto entry = *iter;
        std::string paramName = "/" + m_tf_prefix + "/" + entry.group + "/" + entry.name;
        switch (entry.type) {
          case Crazyflie::ParamTypeUint8:
            ros::param::set(paramName, m_cf.getParam<uint8_t>(entry.id));
            break;
          case Crazyflie::ParamTypeInt8:
            ros::param::set(paramName, m_cf.getParam<int8_t>(entry.id));
            break;
          case Crazyflie::ParamTypeUint16:
            ros::param::set(paramName, m_cf.getParam<uint16_t>(entry.id));
            break;
          case Crazyflie::ParamTypeInt16:
            ros::param::set(paramName, m_cf.getParam<int16_t>(entry.id));
            break;
          case Crazyflie::ParamTypeUint32:
            ros::param::set(paramName, (int)m_cf.getParam<uint32_t>(entry.id));
            break;
          case Crazyflie::ParamTypeInt32:
            ros::param::set(paramName, m_cf.getParam<int32_t>(entry.id));
            break;
          case Crazyflie::ParamTypeFloat:
            ros::param::set(paramName, m_cf.getParam<float>(entry.id));
            break;
        }
      }
    }

    std::unique_ptr<LogBlock<logImu> > logBlockImu;
    std::unique_ptr<LogBlock<log2> > logBlock2;
    std::vector<std::unique_ptr<LogBlockGeneric> > logBlocksGeneric(m_logBlocks.size());
    if (m_enableLogging) {
      ROS_INFO("Requesting Logging variables...");
      m_cf.requestLogToc();

      std::function<void(logImu*)> cb = std::bind(&CrazyflieROS::onImuData, this, std::placeholders::_1);

      logBlockImu.reset(new LogBlock<logImu>(
        &m_cf,{
          {"acc", "x"},
          {"acc", "y"},
          {"acc", "z"},
          {"gyro", "x"},
          {"gyro", "y"},
          {"gyro", "z"},
        }, cb));
      logBlockImu->start(1); // 10ms

      std::function<void(log2*)> cb2 = std::bind(&CrazyflieROS::onLog2Data, this, std::placeholders::_1);

      logBlock2.reset(new LogBlock<log2>(
        &m_cf,{
          {"mag", "x"},
          {"mag", "y"},
          {"mag", "z"},
          {"baro", "temp"},
          {"baro", "pressure"},
          {"pm", "vbat"},
        }, cb2));
      logBlock2->start(10); // 100ms

      // custom log blocks
      size_t i = 0;
      for (auto& logBlock : m_logBlocks)
      {
        std::function<void(std::vector<double>*, void* userData)> cb =
          std::bind(
            &CrazyflieROS::onLogCustom,
            this,
            std::placeholders::_1,
            std::placeholders::_2);

        logBlocksGeneric[i].reset(new LogBlockGeneric(
          &m_cf,
          logBlock.variables,
          (void*)&m_pubLogDataGeneric[i],
          cb));
        logBlocksGeneric[i]->start(logBlock.frequency / 10);
        ++i;
      }


    }

    ROS_INFO("Ready...");
    auto end = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsedSeconds = end-start;
    ROS_INFO("Elapsed: %f s", elapsedSeconds.count());

    // Send 0 thrust initially for thrust-lock
    for (int i = 0; i < 100; ++i) {
       m_cf.sendSetpoint(0, 0, 0, 0, 0 ,0 , 0 , 0);
    }

    while(!m_isEmergency) {
      // make sure we ping often enough to stream data out
      if (m_enableLogging && !m_sentSetpoint) {
        m_cf.sendPing();
      }
      m_sentSetpoint = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Make sure we turn the engines off
    for (int i = 0; i < 100; ++i) {
       m_cf.sendSetpoint(0, 0, 0, 0, 0 , 0 , 0, 0);
    }

  }

  void onImuData(logImu* data) {
    sensor_msgs::Imu msg;
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = m_tf_prefix + "/base_link";
    msg.orientation_covariance[0] = -1;

    // measured in deg/s; need to convert to rad/s
    msg.angular_velocity.x = degToRad(data->gyro_x);
    msg.angular_velocity.y = degToRad(data->gyro_y);
    msg.angular_velocity.z = degToRad(data->gyro_z);

    // measured in mG; need to convert to m/s^2
    msg.linear_acceleration.x = data->acc_x * 9.81;
    msg.linear_acceleration.y = data->acc_y * 9.81;
    msg.linear_acceleration.z = data->acc_z * 9.81;

    m_pubImu.publish(msg);
  }

  void onLog2Data(log2* data) {

    {
      sensor_msgs::Temperature msg;
      msg.header.stamp = ros::Time::now();
      msg.header.frame_id = m_tf_prefix + "/base_link";
      // measured in degC
      msg.temperature = data->baro_temp;
      m_pubTemp.publish(msg);
    }

    {
      sensor_msgs::MagneticField msg;
      msg.header.stamp = ros::Time::now();
      msg.header.frame_id = m_tf_prefix + "/base_link";

      // measured in Tesla
      msg.magnetic_field.x = data->mag_x;
      msg.magnetic_field.y = data->mag_y;
      msg.magnetic_field.z = data->mag_z;
      m_pubMag.publish(msg);
    }

    {
      std_msgs::Float32 msg;
      // hPa (=mbar)
      msg.data = data->baro_pressure;
      m_pubPressure.publish(msg);
    }

    {
      std_msgs::Float32 msg;
      // V
      msg.data = data->pm_vbat;
      m_pubBattery.publish(msg);
    }
  }

  void onLogCustom(std::vector<double>* values, void* userData) {

    ros::Publisher* pub = reinterpret_cast<ros::Publisher*>(userData);

    crazyflie_driver::GenericLogData msg;
    msg.values = *values;

    pub->publish(msg);
  }

private:
  Crazyflie m_cf;
  std::string m_tf_prefix;
  bool m_isEmergency;
  float m_roll_trim;
  float m_pitch_trim;
  bool m_enableLogging;
  bool m_enableParameters;
  std::vector<crazyflie_driver::LogBlock> m_logBlocks;

  ros::ServiceServer m_serviceEmergency;
  ros::ServiceServer m_serviceUpdateParams;
  ros::Subscriber m_subscribeCmdVel;
  ros::Publisher m_pubImu;
  ros::Publisher m_pubTemp;
  ros::Publisher m_pubMag;
  ros::Publisher m_pubPressure;
  ros::Publisher m_pubBattery;
  std::vector<ros::Publisher> m_pubLogDataGeneric;

  bool m_sentSetpoint;
};

bool add_crazyflie(
  crazyflie_driver::AddCrazyflie::Request  &req,
  crazyflie_driver::AddCrazyflie::Response &res)
{
  ROS_INFO("Adding %s as %s with trim(%f, %f). Logging: %d, Parameters: %d",
    req.uri.c_str(),
    req.tf_prefix.c_str(),
    req.roll_trim,
    req.pitch_trim,
    req.enable_parameters,
    req.enable_logging);

  // Leak intentionally
  CrazyflieROS* cf = new CrazyflieROS(
    req.uri,
    req.tf_prefix,
    req.roll_trim,
    req.pitch_trim,
    req.enable_logging,
    req.enable_parameters,
    req.log_blocks);

  return true;
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "crazyflie_server");
  ros::NodeHandle n;

  ros::ServiceServer service = n.advertiseService("add_crazyflie", add_crazyflie);
  ros::spin();

  return 0;
}
