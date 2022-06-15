# 脚本名称: formation_control
# 脚本描述: 该脚本为集群控制demo启动脚本,包含PX4 SITL,Gazebo仿真环境,无人机控制节点以及集群控制节点

# 启动PX4 SITL及Gazebo仿真环境
roslaunch prometheus_gazebo sitl_outdoor_4uav.launch & sleep 5s;
# 启动无人机控制 - uav_control_main
roslaunch prometheus_uav_control uav_control_main_outdoor_4uav.launch & sleep 5s;
# 启动demo (C语言版本)
roslaunch prometheus_demo formation_control.launch



