#!/usr/bin/env python3
"""
nav2_bridge_node.py
===================
Nav2 integration bridge for the Warehouse Automation System.
Runs on Raspberry Pi 5.

This node maps RFID-tag-based landmarks into real-world 2D poses and
sends NavigateToPose action goals to the Nav2 stack. It also publishes
a simplified occupancy grid for the warehouse floor map.

Architecture:
  - The AGV publishes RFID floor tags → /warehouse/agv/location
  - This node interprets that as a landmark fix for localisation
  - When the mission manager requests a rack destination, this node
    converts it to a 2D Nav2 goal pose and dispatches via action client

Nav2 Topics Interfaced:
  /navigate_to_pose    (action)   NavigateToPose
  /initialpose                    geometry_msgs/PoseWithCovarianceStamped
  /warehouse/agv/cmd_vel          geometry_msgs/Twist  (Nav2 output → AGV)
  /warehouse/nav2/goal_reached    std_msgs/String      (feedback to manager)

Landmark Coordinate System:
  All coordinates are in metres, origin at warehouse entry/home position.
  Adjust the LANDMARK_MAP dict to match your physical layout.
"""

import rclpy
from rclpy.node import Node
from rclpy.action import ActionClient
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
import math
import json

from std_msgs.msg import String
from geometry_msgs.msg import (
    Twist, PoseStamped, PoseWithCovarianceStamped,
    Pose, Point, Quaternion
)
from nav_msgs.msg import Odometry
from nav2_msgs.action import NavigateToPose
from action_msgs.msg import GoalStatus


def _yaw_to_quaternion(yaw: float) -> Quaternion:
    """Convert a yaw angle (radians) to a geometry_msgs Quaternion."""
    q = Quaternion()
    q.w = math.cos(yaw / 2.0)
    q.x = 0.0
    q.y = 0.0
    q.z = math.sin(yaw / 2.0)
    return q


# ---------------------------------------------------------------------------
# Warehouse Landmark Map
# Maps RFID tag IDs → (x_m, y_m, yaw_rad) in the warehouse frame.
# EDIT THESE VALUES to match your physical warehouse layout.
# Origin (0,0) = Home/Pickup position near the conveyor.
# ---------------------------------------------------------------------------
LANDMARK_MAP = {
    'LOC-HOME':    (0.00,  0.00,  0.00),   # Home / Pickup point
    'LOC-RACK-A':  (3.00,  0.00,  0.00),   # Rack A: 3m ahead
    'LOC-RACK-B':  (3.00,  1.50,  1.57),   # Rack B: 3m ahead, 1.5m right, face left
    'LOC-RACK-C':  (3.00, -1.50, -1.57),   # Rack C: 3m ahead, 1.5m left, face right
    'LOC-PICKUP':  (0.50,  0.00,  0.00),   # Pickup zone
}

# Maps rack names to RFID landmark tags (used by manager node requests)
RACK_TO_LANDMARK = {
    'RackA':  'LOC-RACK-A',
    'RackB':  'LOC-RACK-B',
    'RackC':  'LOC-RACK-C',
    'Home':   'LOC-HOME',
    'Pickup': 'LOC-PICKUP',
}


class Nav2BridgeNode(Node):
    """Translates warehouse rack destinations into Nav2 NavigateToPose goals."""

    def __init__(self):
        super().__init__('nav2_bridge')

        reliable_qos = QoSProfile(
            reliability=ReliabilityPolicy.RELIABLE,
            depth=10
        )

        # --- Nav2 Action Client ------------------------------------------
        self._nav_client = ActionClient(
            self, NavigateToPose, 'navigate_to_pose')

        # --- State ---------------------------------------------------------
        self.current_pose_x   = 0.0
        self.current_pose_y   = 0.0
        self.current_pose_yaw = 0.0
        self.active_goal      = False
        self.goal_rack        = ''

        # --- Publishers ----------------------------------------------------
        # Initial pose for AMCL localisation (optional, for AMCL-based Nav2)
        self.initialpose_pub = self.create_publisher(
            PoseWithCovarianceStamped, '/initialpose', 10)

        # Notify manager when goal is reached
        self.goal_reached_pub = self.create_publisher(
            String, '/warehouse/nav2/goal_reached', reliable_qos)

        # --- Subscribers ---------------------------------------------------
        # AGV RFID location → use as landmark fix for pose estimation
        self.create_subscription(
            String, '/warehouse/agv/location',
            self._rfid_landmark_cb, reliable_qos)

        # Manager requests navigation to a rack
        self.create_subscription(
            String, '/warehouse/nav2/navigate_to',
            self._navigate_to_cb, reliable_qos)

        # Odometry from AGV micro-ROS node
        self.create_subscription(
            Odometry, '/warehouse/agv/odom',
            self._odom_cb, reliable_qos)

        self.get_logger().info('Nav2BridgeNode started. Waiting for Nav2...')

        # Wait for Nav2 action server (non-blocking)
        self.create_timer(2.0, self._check_nav2_server)

    # -----------------------------------------------------------------------
    # Nav2 Server Check
    # -----------------------------------------------------------------------

    def _check_nav2_server(self):
        if self._nav_client.server_is_ready():
            self.get_logger().info('Nav2 NavigateToPose action server is READY.')
        else:
            self.get_logger().warn('Waiting for Nav2 NavigateToPose action server...')

    # -----------------------------------------------------------------------
    # Callbacks
    # -----------------------------------------------------------------------

    def _odom_cb(self, msg: Odometry):
        p = msg.pose.pose.position
        self.current_pose_x = p.x
        self.current_pose_y = p.y
        # Extract yaw from quaternion
        q = msg.pose.pose.orientation
        siny_cosp = 2.0 * (q.w * q.z + q.x * q.y)
        cosy_cosp = 1.0 - 2.0 * (q.y * q.y + q.z * q.z)
        self.current_pose_yaw = math.atan2(siny_cosp, cosy_cosp)

    def _rfid_landmark_cb(self, msg: String):
        """
        When the AGV scans an RFID floor tag, use it as an absolute
        position fix to correct odometry drift. Publishes /initialpose
        for the AMCL localiser.
        """
        tag = msg.data.strip()
        if tag not in LANDMARK_MAP:
            return

        x, y, yaw = LANDMARK_MAP[tag]
        self.get_logger().info(
            f'RFID landmark fix: {tag} → ({x:.2f}, {y:.2f}, yaw={math.degrees(yaw):.1f}°)')

        # Publish as initial pose for AMCL
        pose_msg = PoseWithCovarianceStamped()
        pose_msg.header.stamp    = self.get_clock().now().to_msg()
        pose_msg.header.frame_id = 'map'
        pose_msg.pose.pose.position.x  = x
        pose_msg.pose.pose.position.y  = y
        pose_msg.pose.pose.orientation  = _yaw_to_quaternion(yaw)
        # Covariance: small uncertainty since RFID is a hard fix
        pose_msg.pose.covariance[0]  = 0.01   # xx
        pose_msg.pose.covariance[7]  = 0.01   # yy
        pose_msg.pose.covariance[35] = 0.005  # θθ
        self.initialpose_pub.publish(pose_msg)

    def _navigate_to_cb(self, msg: String):
        """
        Receive a rack name from the manager node and navigate there.
        Message format: plain rack name string, e.g. 'RackA'
        """
        rack = msg.data.strip()
        landmark_tag = RACK_TO_LANDMARK.get(rack)

        if not landmark_tag:
            self.get_logger().error(f'Unknown rack destination: {rack}')
            return
        if landmark_tag not in LANDMARK_MAP:
            self.get_logger().error(f'No landmark coordinates for: {landmark_tag}')
            return
        if self.active_goal:
            self.get_logger().warn('Nav2: Goal already active. Ignoring new request.')
            return

        x, y, yaw = LANDMARK_MAP[landmark_tag]
        self.goal_rack = rack
        self.get_logger().info(
            f'Navigating to {rack} ({landmark_tag}): ({x:.2f}, {y:.2f})')
        self._send_nav2_goal(x, y, yaw)

    # -----------------------------------------------------------------------
    # Nav2 Goal Dispatch
    # -----------------------------------------------------------------------

    def _send_nav2_goal(self, x: float, y: float, yaw: float):
        if not self._nav_client.server_is_ready():
            self.get_logger().error('Nav2 server not ready. Cannot send goal.')
            return

        goal_msg = NavigateToPose.Goal()
        goal_msg.pose.header.frame_id = 'map'
        goal_msg.pose.header.stamp    = self.get_clock().now().to_msg()
        goal_msg.pose.pose.position.x  = x
        goal_msg.pose.pose.position.y  = y
        goal_msg.pose.pose.orientation  = _yaw_to_quaternion(yaw)

        self.active_goal = True
        send_goal_future = self._nav_client.send_goal_async(
            goal_msg,
            feedback_callback=self._feedback_cb)
        send_goal_future.add_done_callback(self._goal_response_cb)

    def _goal_response_cb(self, future):
        goal_handle = future.result()
        if not goal_handle.accepted:
            self.get_logger().warn('Nav2 goal was REJECTED.')
            self.active_goal = False
            return
        self.get_logger().info('Nav2 goal ACCEPTED. AGV navigating...')
        result_future = goal_handle.get_result_async()
        result_future.add_done_callback(self._result_cb)

    def _feedback_cb(self, feedback_msg):
        fb = feedback_msg.feedback
        dist = fb.distance_remaining
        self.get_logger().debug(f'Nav2 feedback: {dist:.2f}m remaining')

    def _result_cb(self, future):
        result = future.result()
        status = result.status
        self.active_goal = False

        if status == GoalStatus.STATUS_SUCCEEDED:
            self.get_logger().info(f'Nav2: Reached {self.goal_rack}!')
            notify = String()
            notify.data = self.goal_rack
            self.goal_reached_pub.publish(notify)
        else:
            self.get_logger().error(f'Nav2: Goal failed with status {status}')
            notify = String()
            notify.data = f'FAILED:{self.goal_rack}'
            self.goal_reached_pub.publish(notify)
        self.goal_rack = ''


def main(args=None):
    rclpy.init(args=args)
    node = Nav2BridgeNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        node.get_logger().info('Shutting down Nav2BridgeNode.')
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
