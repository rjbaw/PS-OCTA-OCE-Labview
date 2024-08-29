import os
import yaml
from pathlib import Path

from launch import LaunchDescription
from launch.actions import (
    DeclareLaunchArgument,
    RegisterEventHandler,
    IncludeLaunchDescription, #
)
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import (
    Command,
    FindExecutable,
    LaunchConfiguration,
    PathJoinSubstitution
)

from launch.conditions import IfCondition
from launch.event_handlers import OnProcessExit

from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare

from moveit_configs_utils import MoveItConfigsBuilder

from ament_index_python.packages import get_package_share_directory


def load_yaml(package_name, file_path):
    package_path = get_package_share_directory(package_name)
    absolute_file_path = os.path.join(package_path, file_path)

    try:
        with open(absolute_file_path) as file:
            return yaml.safe_load(file)
    except OSError:  # parent of IOError, OSError *and* WindowsError where available
        return None


def generate_launch_description():
    launch_rviz = LaunchConfiguration("launch_rviz")
    ur_type = LaunchConfiguration("ur_type")
    warehouse_sqlite_path = LaunchConfiguration("warehouse_sqlite_path")
    launch_servo = LaunchConfiguration("launch_servo")
    use_sim_time = LaunchConfiguration("use_sim_time")
    publish_robot_description_semantic = LaunchConfiguration("publish_robot_description_semantic")

    # safety_limits = LaunchConfiguration("safety_limits")
    # safety_pos_margin = LaunchConfiguration("safety_pos_margin")
    # safety_k_position = LaunchConfiguration("safety_k_position")
    # description_file = LaunchConfiguration("description_file")
    # tf_prefix = LaunchConfiguration("tf_prefix")
    # robot_ip = LaunchConfiguration("robot_ip")
    # joint_limit = LaunchConfiguration("joint_limit")
    # kinematics_params = LaunchConfiguration("kinematics_params")
    # physical_params = LaunchConfiguration("physical_params")
    # visual_params = LaunchConfiguration("visual_params")
    # robot_description = Command(
    #     [
    #         PathJoinSubstitution([FindExecutable(name="xacro")]),
    #         " ",
    #         description_file,
    #         " ",
    #         "robot_ip:=",
    #         robot_ip,
    #         " ",
    #         "joint_limit_params:=",
    #         joint_limit,
    #         " ",
    #         "kinematics_params:=",
    #         kinematics_params,
    #         " ",
    #         "physical_params:=",
    #         physical_params,
    #         " ",
    #         "visual_params:=",
    #         visual_params,
    #         " ",
    #         "safety_pos_margin:=",
    #         safety_pos_margin,
    #         " ",
    #         "safety_k_position:=",
    #         safety_k_position,
    #         " ",
    #         "name:=",
    #         "ur",
    #         " ",
    #         "ur_type:=",
    #         ur_type,
    #         " ",
    #         "prefix:=",
    #         "''",
    #         " ",
    #         "tf_prefix:=",
    #         tf_prefix,
    #     ]
    # )
    # robot_description_semantic = Command(
    #     [
    #         PathJoinSubstitution([FindExecutable(name="xacro")]),
    #         " ",
    #         PathJoinSubstitution([FindPackageShare("ur_moveit_config"), "srdf", "ur.srdf.xacro"]),
    #         " ",
    #         "name:=",
    #         "ur",
    #         # Also ur_type parameter could be used but then the planning group names in yaml
    #         # configs has to be updated!
    #         " ",
    #         "prefix:=",
    #         "''",
    #         " ",
    #     ]
    # )
    # robot_description = {"robot_description": robot_description}
    # robot_description_semantic = {
    #     "robot_description_semantic": robot_description_semantic
    # }
    # joint_state_publisher_node = Node(
    #     package="joint_state_publisher_gui",
    #     executable="joint_state_publisher_gui",
    # )
    # robot_state_publisher_node = Node(
    #     package="robot_state_publisher",
    #     executable="robot_state_publisher",
    #     output="both",
    #     parameters=[robot_description],
    # )
    # rviz_node = Node(
    #     package="rviz2",
    #     executable="rviz2",
    #     name="rviz2",
    #     output="log",
    #     arguments=["-d", rviz_config_file],
    # )
    # nodes_to_start = [
    #     joint_state_publisher_node,
    #     robot_state_publisher_node,
    #     rviz_node,
    # ]
    # return LaunchDescription(declared_arguments + nodes_to_start)

    moveit_config = (
        MoveItConfigsBuilder(robot_name="ur", package_name="ur_moveit_config")
        .robot_description(Path("urdf") / "ur.urdf.xacro")
        .robot_description_semantic(Path("srdf") / "ur.srdf.xacro", {"name": ur_type})
        .robot_description_kinematics(Path("config") / "kinematics.yaml")
        .joint_limits(Path("config") / "joint_limits.yaml")
        # .planning_pipelines(
        #     pipelines=["ompl", "pilz_industrial_motion_planner"],
        #     default_planning_pipeline="pilz_industrial_motion_planner",
        # )
        .to_moveit_configs()
    )

    warehouse_ros_config = {
        "warehouse_plugin": "warehouse_ros_sqlite::DatabaseConnection",
        "warehouse_host": warehouse_sqlite_path,
    }
    ld = LaunchDescription()
    ld.add_entity(
        LaunchDescription(
            [
                DeclareLaunchArgument(
                    "launch_rviz",
                    default_value="true",
                    description="Launch RViz?"
                ),
                DeclareLaunchArgument(
                    "ur_type",
                    description="Typo/series of used UR robot.",
                    choices=[
                        "ur3",
                        "ur3e",
                        "ur5",
                        "ur5e",
                        "ur10",
                        "ur10e",
                        "ur16e",
                        "ur20",
                        "ur30",
                    ],
                ),
                DeclareLaunchArgument(
                    "warehouse_sqlite_path",
                    default_value=os.path.expanduser("~/.ros/warehouse_ros.sqlite"),
                    description="Path where the warehouse database should be stored",
                ),
                DeclareLaunchArgument(
                    "launch_servo", default_value="false", description="Launch Servo?"
                ),
                DeclareLaunchArgument(
                    "use_sim_time",
                    default_value="false",
                    description="Using or not time from simulation",
                ),
                DeclareLaunchArgument(
                    "publish_robot_description_semantic",
                    default_value="true",
                    description="MoveGroup publishes robot description semantic",
                ),
                DeclareLaunchArgument(
                    "joint_limit",
                    default_value=PathJoinSubstitution(
                        [FindPackageShare("ur_description"),
                        "config", "ur3e", "joint_limits.yaml"]
                    ),
                    description="joint",
                ),
                DeclareLaunchArgument(
                    "kinematics_params",
                    default_value=PathJoinSubstitution(
                        [FindPackageShare("ur_description"),
                         "config", "ur3e", "default_kinematics.yaml"]
                    ),
                    description="joint",
                ),
                DeclareLaunchArgument(
                    "physical_params",
                    default_value=PathJoinSubstitution(
                        [FindPackageShare("ur_description"),
                         "config", "ur3e", "physical_parameters.yaml"]
                    ),
                    description="joint",
                ),
                DeclareLaunchArgument(
                    "visual_params",
                    default_value=PathJoinSubstitution(
                        [FindPackageShare("ur_description"),
                         "config", "ur3e", "visual_parameters.yaml"]
                    ),
                    description="joint",
                ),
                DeclareLaunchArgument(
                    "tf_prefix",
                    default_value="''",
                    description="Prefix of the joint names, useful for "
                    "multi-robot setup. If changed than also joint names in the controllers' configuration "
                    "have to be updated.",
                ),
                DeclareLaunchArgument(
                    "safety_limits",
                    default_value="true",
                    description="Enables the safety limits controller if true.",
                ),
                DeclareLaunchArgument(
                    "safety_pos_margin",
                    default_value="0.15",
                    description="The margin to lower and upper limits in the safety controller.",
                ),
                DeclareLaunchArgument(
                    "safety_k_position",
                    default_value="20",
                    description="k-position factor in the safety controller.",
                ),
                DeclareLaunchArgument(
                    "description_file",
                    default_value=PathJoinSubstitution(
                        [FindPackageShare("ur_description"), "urdf", "ur.urdf.xacro"]
                    ),
                    description="URDF/XACRO description file (absolute path) with the robot.",
                ),
                DeclareLaunchArgument(
                    "rviz_config_file",
                    default_value=PathJoinSubstitution(
                        [FindPackageShare("ur_description"), "rviz", "view_robot.rviz"]
                    ),
                    description="RViz config file (absolute path) to use when launching rviz.",
                ),
            ]
        )
    )

    wait_robot_description = Node(
        package="ur_robot_driver",
        executable="wait_for_robot_description",
        output="screen",
    )
    ld.add_action(wait_robot_description)

    move_group_node = Node(
        package="moveit_ros_move_group",
        executable="move_group",
        output="screen",
        parameters=[
            moveit_config.to_dict(),
            warehouse_ros_config,
            {
                "use_sim_time": use_sim_time,
                "publish_robot_description_semantic": publish_robot_description_semantic,
            },
        ],
    )

    servo_yaml = load_yaml("ur_moveit_config", "config/ur_servo.yaml")
    servo_params = {"moveit_servo": servo_yaml}
    servo_node = Node(
        package="moveit_servo",
        condition=IfCondition(launch_servo),
        executable="servo_node",
        parameters=[
            moveit_config.to_dict(),
            servo_params,
        ],
        output="screen",
    )

    rviz_config_file = PathJoinSubstitution(
        [FindPackageShare("ur_moveit_config"), "config", "moveit.rviz"]
    )
    rviz_node = Node(
        package="rviz2",
        condition=IfCondition(launch_rviz),
        executable="rviz2",
        name="rviz2_moveit",
        output="log",
        arguments=["-d", rviz_config_file],
        parameters=[
            moveit_config.robot_description,
            moveit_config.robot_description_semantic,
            moveit_config.robot_description_kinematics,
            moveit_config.planning_pipelines,
            moveit_config.joint_limits,
            warehouse_ros_config,
        ],
    )

    robot_state_node = Node(
        package="octa_ros",
        executable="joint_state_listener",
        name="joint_state_listener",
        output="screen",
    )

    ld.add_action(
        RegisterEventHandler(
            OnProcessExit(
                target_action=wait_robot_description,
                on_exit=[move_group_node, rviz_node, servo_node, robot_state_node],
            )
        ),
    )

    return ld
