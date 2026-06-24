#!/usr/bin/env python3
"""
warehouse.launch.py
===================
Main launch file for the Warehouse Automation System on Raspberry Pi 5.

Starts:
  1. micro-ROS Agent (UDP, port 8888) — bridges all ESP32 micro-ROS nodes
  2. warehouse_manager  — mission orchestrator
  3. fleet_monitor      — live terminal telemetry dashboard
  4. nav2_bridge        — Nav2 waypoint goal dispatcher (optional)

Usage:
  ros2 launch warehouse_ros2 warehouse.launch.py
  ros2 launch warehouse_ros2 warehouse.launch.py nav2:=false   # skip Nav2
"""

import os
from launch import LaunchDescription
from launch.actions import (
    DeclareLaunchArgument, ExecuteProcess, TimerAction,
    IncludeLaunchDescription, LogInfo
)
from launch.conditions import IfCondition
from launch.substitutions import LaunchConfiguration, FindExecutable
from launch_ros.actions import Node
from ament_index_python.packages import get_package_share_directory


def generate_launch_description():

    # ---------- Launch Arguments -------------------------------------------
    nav2_arg = DeclareLaunchArgument(
        'nav2',
        default_value='true',
        description='Launch Nav2 bridge node (true/false)'
    )

    agent_port_arg = DeclareLaunchArgument(
        'agent_port',
        default_value='8888',
        description='UDP port for micro-ROS Agent'
    )

    log_level_arg = DeclareLaunchArgument(
        'log_level',
        default_value='info',
        description='ROS2 log level (debug/info/warn/error)'
    )

    nav2_enabled = LaunchConfiguration('nav2')
    agent_port   = LaunchConfiguration('agent_port')
    log_level    = LaunchConfiguration('log_level')

    pkg_share = get_package_share_directory('warehouse_ros2')

    # ---------- micro-ROS Agent --------------------------------------------
    # Runs as a system process. Bridges UDP packets from ESP32s to ROS2 DDS.
    # The ESP32 firmwares must have the RPi5's IP set as the agent address.
    microros_agent = ExecuteProcess(
        cmd=[
            'micro-ros-agent', 'udp4',
            '--port', agent_port,
            '-v4'          # verbose level 4 (reduce to 1 for production)
        ],
        name='micro_ros_agent',
        output='screen',
        # Uncomment below to launch via Docker instead:
        # cmd=[
        #     'docker', 'run', '--rm', '--net=host',
        #     'microros/micro-ros-agent:humble',
        #     'udp4', '--port', agent_port, '-v4'
        # ],
    )

    # ---------- Warehouse Manager Node -------------------------------------
    warehouse_manager = Node(
        package='warehouse_ros2',
        executable='warehouse_manager',
        name='warehouse_manager',
        output='screen',
        arguments=['--ros-args', '--log-level', log_level],
        parameters=[
            os.path.join(pkg_share, 'config', 'warehouse_params.yaml')
        ]
    )

    # ---------- Fleet Monitor Node -----------------------------------------
    fleet_monitor = Node(
        package='warehouse_ros2',
        executable='fleet_monitor',
        name='fleet_monitor',
        output='screen',
        arguments=['--ros-args', '--log-level', log_level],
    )

    # ---------- Nav2 Bridge Node (optional) --------------------------------
    nav2_bridge = Node(
        package='warehouse_ros2',
        executable='nav2_bridge',
        name='nav2_bridge',
        output='screen',
        condition=IfCondition(nav2_enabled),
        arguments=['--ros-args', '--log-level', log_level],
    )

    # ---------- Optional: Include Nav2 Bringup ----------------------------
    # Uncomment to auto-start full Nav2 stack with the warehouse map.
    # nav2_bringup_dir = get_package_share_directory('nav2_bringup')
    # nav2_launch = IncludeLaunchDescription(
    #     os.path.join(nav2_bringup_dir, 'launch', 'bringup_launch.py'),
    #     launch_arguments={
    #         'map': os.path.join(pkg_share, 'maps', 'warehouse_map.yaml'),
    #         'params_file': os.path.join(pkg_share, 'config', 'nav2_params.yaml'),
    #         'use_sim_time': 'false',
    #     }.items(),
    #     condition=IfCondition(nav2_enabled),
    # )

    # Delay ROS2 nodes by 2s to let micro-ROS agent initialise first
    delayed_manager = TimerAction(period=2.0, actions=[warehouse_manager])
    delayed_monitor = TimerAction(period=2.5, actions=[fleet_monitor])
    delayed_nav2    = TimerAction(period=3.0, actions=[nav2_bridge])

    return LaunchDescription([
        nav2_arg,
        agent_port_arg,
        log_level_arg,
        LogInfo(msg='=== Warehouse Automation System (ROS2 Humble) ==='),
        LogInfo(msg='Starting micro-ROS UDP agent...'),
        microros_agent,
        delayed_manager,
        delayed_monitor,
        delayed_nav2,
    ])
