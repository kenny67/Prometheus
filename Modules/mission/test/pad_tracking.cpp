
//ROS 头文件
#include <ros/ros.h>
#include <iostream>
#include <sensor_msgs/Range.h>
#include <mission_utils.h>

#include "message_utils.h"

using namespace std;
using namespace Eigen;

#define NODE_NAME "pad_tracking"
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>全 局 变 量<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
bool hold_mode;
string message;
bool sim_mode;
bool tfmini_flag;
std_msgs::Bool vision_switch;
//---------------------------------------Drone---------------------------------------------
prometheus_msgs::DroneState _DroneState;   
Eigen::Matrix3f R_Body_to_ENU;
//---------------------------------------Vision---------------------------------------------
Detection_result landpad_det;
float kp_land[3];         //控制参数 - 比例参数
float start_point_x,start_point_y,start_point_z;

//-----　laser
sensor_msgs::Range tfmini_data;
//---------------------------------------Track---------------------------------------------
// 五种状态机
enum EXEC_STATE
{
    WAITING_RESULT,
    TRACKING,
    LOST,
    LANDING,
};
EXEC_STATE exec_state;

float distance_to_pad;
float arm_height_to_ground;
float arm_distance_to_pad;
nav_msgs::Odometry GroundTruth;
//---------------------------------------Output---------------------------------------------
prometheus_msgs::ControlCommand Command_Now;                               //发送给控制模块 [px4_pos_controller.cpp]的命令
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>函数声明<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
void printf_param();                                                                 //打印各项参数以供检查
void printf_result();
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>回调函数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
void landpad_det_cb(const prometheus_msgs::DetectionInfo::ConstPtr &msg)
{
    landpad_det.object_name = "landpad";
    landpad_det.Detection_info = *msg;
    // 识别算法发布的目标位置位于相机坐标系（从相机往前看，物体在相机右方x为正，下方y为正，前方z为正）
    // 相机安装误差 在mission_utils.h中设置
    landpad_det.pos_body_frame[0] = - landpad_det.Detection_info.position[1] + DOWN_CAMERA_OFFSET_X;
    landpad_det.pos_body_frame[1] = - landpad_det.Detection_info.position[0] + DOWN_CAMERA_OFFSET_Y;
    landpad_det.pos_body_frame[2] = - landpad_det.Detection_info.position[2] + DOWN_CAMERA_OFFSET_Z;

    // 机体系 -> 机体惯性系 (原点在机体的惯性系) (对无人机姿态进行解耦)
    landpad_det.pos_body_enu_frame = R_Body_to_ENU * landpad_det.pos_body_frame;

    if(tfmini_flag)
    {
        // 使用TFmini测得高度
        landpad_det.pos_body_enu_frame[2] = - tfmini_data.range;
    }else
    {
        // 若已知降落板高度，则无需使用深度信息。
        landpad_det.pos_body_enu_frame[2] =  - _DroneState.position[2];
    }
    
    // 机体惯性系 -> 惯性系
    landpad_det.pos_enu_frame[0] = _DroneState.position[0] + landpad_det.pos_body_enu_frame[0];
    landpad_det.pos_enu_frame[1] = _DroneState.position[1] + landpad_det.pos_body_enu_frame[1];
    landpad_det.pos_enu_frame[2] = _DroneState.position[2] + landpad_det.pos_body_enu_frame[2];

    landpad_det.att_enu_frame[2] = 0.0;

    if(landpad_det.Detection_info.detected)
    {
        landpad_det.num_regain++;
        landpad_det.num_lost = 0;
    }else
    {
        landpad_det.num_regain = 0;
        landpad_det.num_lost++;
    }

    // 当连续一段时间无法检测到目标时，认定目标丢失
    if(landpad_det.num_lost > VISION_THRES)
    {
        landpad_det.is_detected = false;
    }

    // 当连续一段时间检测到目标时，认定目标得到
    if(landpad_det.num_regain > VISION_THRES)
    {
        landpad_det.is_detected = true;
    }

}
void groundtruth_cb(const nav_msgs::Odometry::ConstPtr& msg)
{
    GroundTruth = *msg;
}
void drone_state_cb(const prometheus_msgs::DroneState::ConstPtr& msg)
{
    _DroneState = *msg;

    R_Body_to_ENU = get_rotation_matrix(_DroneState.attitude[0], _DroneState.attitude[1], _DroneState.attitude[2]);
}
void tfmini_cb(const sensor_msgs::Range::ConstPtr& msg)
{
    tfmini_data = *msg;
}
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>主函数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
int main(int argc, char **argv)
{
    ros::init(argc, argv, "pad_tracking");
    ros::NodeHandle nh("~");

    //节点运行频率： 20hz 【视觉端解算频率大概为20HZ】
    ros::Rate rate(20.0);

    //【订阅】降落板与无人机的相对位置及相对偏航角  单位：米   单位：弧度
    //  方向定义： 识别算法发布的目标位置位于相机坐标系（从相机往前看，物体在相机右方x为正，下方y为正，前方z为正）
    //  标志位：   detected 用作标志位 ture代表识别到目标 false代表丢失目标
    ros::Subscriber landpad_det_sub = nh.subscribe<prometheus_msgs::DetectionInfo>("/prometheus/object_detection/ellipse_det", 10, landpad_det_cb);

    //【订阅】无人机高度(由tfmini测量得到)
    ros::Subscriber tfmini_sub = nh.subscribe<sensor_msgs::Range>("/prometheus/tfmini_range", 10, tfmini_cb);

    //【订阅】无人机状态
    ros::Subscriber drone_state_sub = nh.subscribe<prometheus_msgs::DroneState>("/prometheus/drone_state", 10, drone_state_cb);

    //【订阅】地面真值，此信息仅做比较使用 不强制要求提供
    ros::Subscriber groundtruth_sub = nh.subscribe<nav_msgs::Odometry>("/ground_truth/landing_pad", 10, groundtruth_cb);

    // 【发布】 视觉模块开关量
    ros::Publisher vision_switch_pub = nh.advertise<std_msgs::Bool>("/prometheus/switch/ellipse_det", 10);

    //【发布】发送给控制模块 [px4_pos_controller.cpp]的命令
    ros::Publisher command_pub = nh.advertise<prometheus_msgs::ControlCommand>("/prometheus/control_command", 10);

    // 【发布】用于地面站显示的提示消息
    ros::Publisher message_pub = nh.advertise<prometheus_msgs::Message>("/prometheus/message/main", 10);

//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>参数读取<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
    //强制上锁高度
    nh.param<float>("arm_height_to_ground", arm_height_to_ground, 0.2);
    //强制上锁距离
    nh.param<float>("arm_distance_to_pad", arm_distance_to_pad, 0.2);
    // 悬停模式 - 仅用于观察检测结果
    nh.param<bool>("hold_mode", hold_mode, false);
    // 仿真模式 - 区别在于是否自动切换offboard模式
    nh.param<bool>("sim_mode", sim_mode, false);
    // 是否使用tfmini_data
    nh.param<bool>("tfmini_flag", tfmini_flag, false);

    //追踪控制参数
    nh.param<float>("kpx_land", kp_land[0], 0.1);
    nh.param<float>("kpy_land", kp_land[1], 0.1);
    nh.param<float>("kpz_land", kp_land[2], 0.1);

    nh.param<float>("start_point_x", start_point_x, 0.0);
    nh.param<float>("start_point_y", start_point_y, 0.0);
    nh.param<float>("start_point_z", start_point_z, 2.0);

#if DEBUG_MODE == 1
    //打印现实检查参数
    printf_param();
#endif

    //固定的浮点显示
    cout.setf(ios::fixed);
    //setprecision(n) 设显示小数精度为n位
    cout<<setprecision(2);
    //左对齐
    cout.setf(ios::left);
    // 强制显示小数点
    cout.setf(ios::showpoint);
    // 强制显示符号
    cout.setf(ios::showpos);


    Command_Now.Command_ID = 1;
    Command_Now.source = NODE_NAME;

    if(sim_mode)
    {
        // Waiting for input
        int start_flag = 0;
        while(start_flag == 0)
        {
            cout << ">>>>>>>>>>>>>>>>>>>>>>>>>Autonomous Landing Mission<<<<<<<<<<<<<<<<<<<<<< "<< endl;
            cout << "Please check the parameter and setting，enter 1 to continue， else for quit: "<<endl;
            cin >> start_flag;
        }

        while(ros::ok() && _DroneState.mode != "OFFBOARD")
        {
            Command_Now.header.stamp = ros::Time::now();
            Command_Now.Mode  = prometheus_msgs::ControlCommand::Idle;
            Command_Now.Command_ID = Command_Now.Command_ID + 1;
            Command_Now.source = NODE_NAME;
            Command_Now.Reference_State.yaw_ref = 999;
            command_pub.publish(Command_Now);   
            cout << "Switch to OFFBOARD and arm ..."<<endl;
            ros::Duration(2.0).sleep();
            ros::spinOnce();
        }
    }else
    {
        while(ros::ok() && _DroneState.mode != "OFFBOARD")
        {
            cout << "Waiting for the offboard mode"<<endl;
            ros::Duration(1.0).sleep();
            ros::spinOnce();
        }
    }
    
    // 起飞
    cout<<"[autonomous_landing]: "<<"Takeoff to predefined position."<<endl;
    pub_message(message_pub, prometheus_msgs::Message::WARN, NODE_NAME, "Takeoff to predefined position.");

    while( _DroneState.position[2] < 0.5)
    {      
        Command_Now.header.stamp                        = ros::Time::now();
        Command_Now.Mode                                = prometheus_msgs::ControlCommand::Move;
        Command_Now.Command_ID                          = Command_Now.Command_ID + 1;
        Command_Now.source                              = NODE_NAME;
        Command_Now.Reference_State.Move_mode           = prometheus_msgs::PositionReference::XYZ_POS;
        Command_Now.Reference_State.Move_frame          = prometheus_msgs::PositionReference::ENU_FRAME;
        Command_Now.Reference_State.position_ref[0]     = start_point_x;
        Command_Now.Reference_State.position_ref[1]     = start_point_y;
        Command_Now.Reference_State.position_ref[2]     = start_point_z;
        Command_Now.Reference_State.yaw_ref             = 0;
        command_pub.publish(Command_Now);
        cout << "Takeoff ..."<<endl;
        ros::Duration(3.0).sleep();

        ros::spinOnce();
    }

    // 等待
    ros::Duration(3.0).sleep();

    exec_state = EXEC_STATE::WAITING_RESULT;

    while (ros::ok())
    {
        //回调
        ros::spinOnce();

        static int printf_num = 0;
        printf_num++;
        if(printf_num > 20)
        {
            printf_result();
            printf_num = 0;
        }

        switch (exec_state)
        {
            case WAITING_RESULT:
            {
                if(landpad_det.is_detected)
                {
                    exec_state = TRACKING;
                    message = "Get the detection result.";
                    cout << message <<endl;
                    pub_message(message_pub, prometheus_msgs::Message::WARN, NODE_NAME, message);
                    break;
                }

                vision_switch.data = true;
                vision_switch_pub.publish(vision_switch);
                message = "Waiting for the detection result.";
                cout << message <<endl;
                pub_message(message_pub, prometheus_msgs::Message::WARN, NODE_NAME, message);
                ros::Duration(1.0).sleep();
                break;
            }
            case TRACKING:
            {
                // 丢失,进入LOST
                if(!landpad_det.is_detected)
                {
                    exec_state = LOST;
                    message = "Lost the Landing Pad.";
                    cout << message <<endl;
                    pub_message(message_pub, prometheus_msgs::Message::WARN, NODE_NAME, message);
                    break;
                }   

                // 抵达上锁点,进入LANDING
                distance_to_pad = landpad_det.pos_body_enu_frame.norm();
                //　达到降落距离，上锁降落
                if(distance_to_pad < arm_distance_to_pad)
                {
                    exec_state = LANDING;
                    message = "Catched the Landing Pad.";
                    cout << message <<endl;
                    pub_message(message_pub, prometheus_msgs::Message::WARN, NODE_NAME, message);
                    break;
                }
                //　达到最低高度，上锁降落
                else if(abs(landpad_det.pos_body_enu_frame[2]) < arm_height_to_ground)
                {
                    exec_state = LANDING;
                    message = "Reach the lowest height.";
                    cout << message <<endl;
                    pub_message(message_pub, prometheus_msgs::Message::WARN, NODE_NAME, message);
                    break;
                }



                // 机体系速度控制
                Command_Now.header.stamp = ros::Time::now();
                Command_Now.Command_ID   = Command_Now.Command_ID + 1;
                Command_Now.source = NODE_NAME;
                Command_Now.Mode = prometheus_msgs::ControlCommand::Move;
                Command_Now.Reference_State.Move_frame = prometheus_msgs::PositionReference::BODY_FRAME;
                Command_Now.Reference_State.Move_mode = prometheus_msgs::PositionReference::XYZ_VEL;   //xy velocity z position
                for (int i=0; i<3; i++)
                {
                    Command_Now.Reference_State.velocity_ref[i] = kp_land[i] * landpad_det.pos_body_enu_frame[i];
                }

                Command_Now.Reference_State.yaw_ref             = 0.0;
                //Publish

                if (!hold_mode)
                {
                    command_pub.publish(Command_Now);
                }

                break;
            }
            case LOST:
            {
                static int lost_time = 0;
                lost_time ++ ;
                
                // 重新获得信息,进入TRACKING
                if(landpad_det.is_detected)
                {
                    exec_state = TRACKING;
                    lost_time = 0;
                    message = "Regain the Landing Pad.";
                    cout << message <<endl;
                    pub_message(message_pub, prometheus_msgs::Message::WARN, NODE_NAME, message);
                    break;
                }   
                
                // 首先是悬停等待 尝试得到图像, 然后重回初始点
                if(lost_time < 5.0)
                {
                    Command_Now.header.stamp = ros::Time::now();
                    Command_Now.Command_ID   = Command_Now.Command_ID + 1;
                    Command_Now.source = NODE_NAME;
                    Command_Now.Mode = prometheus_msgs::ControlCommand::Hold;
                }else
                {
                    Command_Now.header.stamp                        = ros::Time::now();
                    Command_Now.Mode                                = prometheus_msgs::ControlCommand::Move;
                    Command_Now.Command_ID                          = Command_Now.Command_ID + 1;
                    Command_Now.source                              = NODE_NAME;
                    Command_Now.Reference_State.Move_mode           = prometheus_msgs::PositionReference::XYZ_POS;
                    Command_Now.Reference_State.Move_frame          = prometheus_msgs::PositionReference::ENU_FRAME;
                    Command_Now.Reference_State.position_ref[0]     = start_point_x;
                    Command_Now.Reference_State.position_ref[1]     = start_point_y;
                    Command_Now.Reference_State.position_ref[2]     = start_point_z;
                    Command_Now.Reference_State.yaw_ref             = 0;
                }
                command_pub.publish(Command_Now);
                break;
            }
            case LANDING:
            {
                Command_Now.header.stamp = ros::Time::now();
                Command_Now.Command_ID   = Command_Now.Command_ID + 1;
                Command_Now.source = NODE_NAME;
                Command_Now.Mode = prometheus_msgs::ControlCommand::Disarm;
                command_pub.publish(Command_Now);

                ros::Duration(1.0).sleep();

                break;
            }
        }
      
        rate.sleep();

    }

    return 0;

}


void printf_result()
{
    cout << ">>>>>>>>>>>>>>>>>>>>>>Autonomous Landing Mission<<<<<<<<<<<<<<<<<<<"<< endl;

    cout << ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>Vision State<<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;
    if(landpad_det.is_detected)
    {
        cout << "is_detected: ture" <<endl;
    }else
    {
        cout << "is_detected: false" <<endl;
    }
    
    cout << "Detection_raw(pos): " << landpad_det.pos_body_frame[0] << " [m] "<< landpad_det.pos_body_frame[1] << " [m] "<< landpad_det.pos_body_frame[2] << " [m] "<<endl;
    cout << "Detection_raw(yaw): " << landpad_det.Detection_info.yaw_error/3.1415926 *180 << " [deg] "<<endl;

    if( exec_state == TRACKING)
    {
        // 正常追踪
        cout <<"[autonomous_landing]: Tracking the Landing Pad, distance_to_pad : "<< distance_to_pad << " [m] " << endl;
    }



#if DEBUG_MODE == 1
    cout << "Ground_truth(pos):  " << GroundTruth.pose.pose.position.x << " [m] "<< GroundTruth.pose.pose.position.y << " [m] "<< GroundTruth.pose.pose.position.z << " [m] "<<endl;
    cout << "Detection_ENU(pos): " << landpad_det.pos_enu_frame[0] << " [m] "<< landpad_det.pos_enu_frame[1] << " [m] "<< landpad_det.pos_enu_frame[2] << " [m] "<<endl;
    cout << "Detection_ENU(yaw): " << landpad_det.att_enu_frame[2]/3.1415926 *180 << " [deg] "<<endl;
#endif

    cout <<">>>>>>>>>>>>>>>>>>>>>>>>>Land Control State<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;
    // cout << "pos_des: " << Command_Now.Reference_State.position_ref[0] << " [m] "<< Command_Now.Reference_State.position_ref[1] << " [m] "<< Command_Now.Reference_State.position_ref[2] << " [m] "<<endl;

    cout << "vel_cmd: " << Command_Now.Reference_State.velocity_ref[0] << " [m/s] "<< Command_Now.Reference_State.velocity_ref[1] << " [m/s] "<< Command_Now.Reference_State.velocity_ref[2] << " [m/s] "<<endl;
}
void printf_param()
{
    cout <<">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> Parameter <<<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;
    cout << "arm_distance_to_pad : "<< arm_distance_to_pad << endl;
    cout << "arm_height_to_ground : "<< arm_height_to_ground << endl;
    cout << "kpx_land : "<< kp_land[0] << endl;
    cout << "kpy_land : "<< kp_land[1] << endl;
    cout << "kpz_land : "<< kp_land[2] << endl;
    cout << "start_point_x : "<< start_point_x << endl;
    cout << "start_point_y : "<< start_point_y << endl;
    cout << "start_point_z : "<< start_point_z << endl;
}

