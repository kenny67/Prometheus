#ifndef UGV_CONTROLLER_H
#define UGV_CONTROLLER_H
#include <ros/ros.h>
#include <bitset>

#include <nav_msgs/Odometry.h>
#include <geometry_msgs/Twist.h>
#include <prometheus_msgs/UGVState.h>
#include <prometheus_msgs/MultiUGVState.h>
#include <prometheus_msgs/UGVCommand.h>

#include "math_utils.h"
#include "Smoother.h"
#include "angles/angles.h"  // vinson: shortest_angular_distance
#include "printf_utils.h"

// 宏定义
#define NUM_POINT 2                             // 打印小数点
using namespace std;

class UGV_controller
{
public:

    UGV_controller(ros::NodeHandle& nh);
    void mainloop();



    // 变量
    int ugv_id,swarm_num_ugv,id;                                               // 无人车编号
    string ugv_name;                                // 无人车名字
    Eigen::Vector2f geo_fence_x,geo_fence_y;                           //Geigraphical fence 地理围栏
    prometheus_msgs::UGVCommand Command_Now;      // 无人车当前执行命令
    prometheus_msgs::UGVCommand Command_Last;       // 无人车上一条执行命令
    prometheus_msgs::UGVState _UgvState;                             // 无人车状态
    Eigen::Vector3d pos_ugv;                      // 无人车位置
    Eigen::Vector3d vel_ugv;                      // 无人车速度



    float yaw_ugv;                                          // 无人车偏航角
    float k_p,k_p_path,k_yaw,k_aoivd;                                      // 速度控制参数
    float max_vel;                                          // 无人车最大速度
    bool flag_printf;                                      // 是否打印
    float error_yaw;
    geometry_msgs::Twist cmd_vel;      // 底层速度指令   

    Eigen::Vector2f vel_avoid_nei;


    // 订阅
    ros::Subscriber command_sub;
    ros::Subscriber ugv_state_sub;
    
    // 发布
    ros::Publisher cmd_pub;

    double dist,direct,yaw_error;
    bool only_rotate;
    bool first_arrive;  // 在距离上到达并停下之前都为false
    bool stop_flag;

    float test_time;

    float delta_t{0.1};
    float ugv_arr_circle_t_{0};


private:
    void ugv_command_cb(const prometheus_msgs::UGVCommand::ConstPtr& msg);
    void ugv_state_cb(const prometheus_msgs::UGVState::ConstPtr& msg);
    // void add_apf_vel();
    void Circle_trajectory_generation(float time_from_start, int id);
    void printf_state(const ros::TimerEvent &e);
    int check_failsafe();
    void CalErrorYaw();
    void VelLimit();

};


#endif