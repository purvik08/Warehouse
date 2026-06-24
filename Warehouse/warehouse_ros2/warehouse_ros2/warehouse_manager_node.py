#!/usr/bin/env python3
"""
warehouse_manager_node.py
=========================
Main ROS2 orchestrator for the Warehouse Automation System.
Runs on Raspberry Pi 5 with ROS2 Humble.

Responsibilities:
  - Receives all telemetry from micro-ROS ESP32 nodes via DDS
  - Manages the warehouse state machine (IDLE → MISSION → COMPLETE)
  - Issues movement commands to the AGV
  - Issues pick/place commands to the Robotic Arm
  - Triggers Nav2 waypoint navigation based on inventory logic
  - Publishes system diagnostics

Topic Map (all under /warehouse/):
  Subscribed:
    /warehouse/agv/status          std_msgs/String
    /warehouse/agv/battery         sensor_msgs/BatteryState
    /warehouse/agv/location        std_msgs/String   (RFID floor tag)
    /warehouse/agv/obstacle        std_msgs/Bool
    /warehouse/arm/status          std_msgs/String
    /warehouse/server/rfid         std_msgs/String   (box scan from server RFID)
    /warehouse/server/inventory    std_msgs/String   (JSON inventory snapshot)

  Published:
    /warehouse/agv/cmd_vel         geometry_msgs/Twist   (Nav2 compatible)
    /warehouse/agv/command         std_msgs/String       (direct string cmd)
    /warehouse/arm/command         std_msgs/String
    /warehouse/mission/status      std_msgs/String       (JSON mission state)
    /diagnostics                   diagnostic_msgs/DiagnosticArray
"""

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
import json
import time

from std_msgs.msg import String, Bool
from geometry_msgs.msg import Twist
from sensor_msgs.msg import BatteryState
from diagnostic_msgs.msg import DiagnosticArray, DiagnosticStatus, KeyValue


# ---------------------------------------------------------------------------
# Warehouse state machine states
# ---------------------------------------------------------------------------
class MissionState:
    IDLE         = 'IDLE'
    BOX_DETECTED = 'BOX_DETECTED'
    ARM_PICKING  = 'ARM_PICKING'
    AGV_LOADING  = 'AGV_LOADING'
    AGV_MOVING   = 'AGV_MOVING'
    AGV_ARRIVED  = 'AGV_ARRIVED'
    ARM_PLACING  = 'ARM_PLACING'
    COMPLETE     = 'COMPLETE'
    ERROR        = 'ERROR'


# ---------------------------------------------------------------------------
# RFID waypoint map — maps RFID tag IDs to rack destinations
# Update these with the actual tag values written to your MIFARE cards.
# ---------------------------------------------------------------------------
RFID_WAYPOINT_MAP = {
    'LOC-RACK-A': 'RackA',
    'LOC-RACK-B': 'RackB',
    'LOC-RACK-C': 'RackC',
    'LOC-PICKUP':  'Pickup',
    'LOC-HOME':    'Home',
}

# Box RFID prefix → target rack routing
BOX_RACK_MAP = {
    'BOXA': 'RackA',
    'BOXB': 'RackB',
    'BOXC': 'RackC',
}


class WarehouseManagerNode(Node):
    """Main orchestrator node running on RPi5."""

    def __init__(self):
        super().__init__('warehouse_manager')

        # --- QoS profiles -----------------------------------------------
        reliable_qos = QoSProfile(
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.VOLATILE,
            depth=10
        )
        sensor_qos = QoSProfile(
            reliability=ReliabilityPolicy.BEST_EFFORT,
            depth=5
        )

        # --- State ----------------------------------------------------------
        self.mission_state   = MissionState.IDLE
        self.current_box_tag = ''
        self.target_rack     = ''
        self.agv_status      = 'UNKNOWN'
        self.agv_battery     = 0.0
        self.agv_location    = 'UNKNOWN'
        self.agv_obstacle    = False
        self.arm_status      = 'UNKNOWN'
        self.last_agv_seen   = 0.0
        self.last_arm_seen   = 0.0

        # --- Subscribers ----------------------------------------------------
        self.create_subscription(
            String, '/warehouse/agv/status',
            self._agv_status_cb, reliable_qos)

        self.create_subscription(
            BatteryState, '/warehouse/agv/battery',
            self._agv_battery_cb, sensor_qos)

        self.create_subscription(
            String, '/warehouse/agv/location',
            self._agv_location_cb, reliable_qos)

        self.create_subscription(
            Bool, '/warehouse/agv/obstacle',
            self._agv_obstacle_cb, sensor_qos)

        self.create_subscription(
            String, '/warehouse/arm/status',
            self._arm_status_cb, reliable_qos)

        self.create_subscription(
            String, '/warehouse/server/rfid',
            self._server_rfid_cb, reliable_qos)

        self.create_subscription(
            String, '/warehouse/server/inventory',
            self._server_inventory_cb, reliable_qos)

        # --- Publishers -----------------------------------------------------
        self.agv_cmd_vel_pub = self.create_publisher(
            Twist, '/warehouse/agv/cmd_vel', reliable_qos)

        self.agv_command_pub = self.create_publisher(
            String, '/warehouse/agv/command', reliable_qos)

        self.arm_command_pub = self.create_publisher(
            String, '/warehouse/arm/command', reliable_qos)

        self.mission_status_pub = self.create_publisher(
            String, '/warehouse/mission/status', reliable_qos)

        self.diagnostics_pub = self.create_publisher(
            DiagnosticArray, '/diagnostics', 10)

        # --- Timers ---------------------------------------------------------
        # State machine tick: 500ms
        self.create_timer(0.5, self._state_machine_tick)
        # Diagnostics: every 5s
        self.create_timer(5.0, self._publish_diagnostics)
        # Mission status broadcast: every 1s
        self.create_timer(1.0, self._publish_mission_status)

        self.get_logger().info('WarehouseManagerNode started. State: IDLE')

    # -----------------------------------------------------------------------
    # Subscriber Callbacks
    # -----------------------------------------------------------------------

    def _agv_status_cb(self, msg: String):
        self.agv_status  = msg.data
        self.last_agv_seen = time.time()
        self.get_logger().debug(f'AGV status: {msg.data}')

    def _agv_battery_cb(self, msg: BatteryState):
        # BatteryState.percentage is 0.0–1.0
        self.agv_battery = msg.percentage * 100.0

    def _agv_location_cb(self, msg: String):
        """Called when AGV scans a floor RFID tag."""
        tag = msg.data.strip()
        self.agv_location = tag
        self.get_logger().info(f'AGV location update: {tag}')

        # Check if AGV arrived at target rack
        if self.mission_state == MissionState.AGV_MOVING:
            target_tag = self._rack_to_rfid_tag(self.target_rack)
            if tag == target_tag:
                self.get_logger().info(f'AGV arrived at {self.target_rack}!')
                self.mission_state = MissionState.AGV_ARRIVED
                self._send_agv_command('stop')

    def _agv_obstacle_cb(self, msg: Bool):
        if msg.data and not self.agv_obstacle:
            self.get_logger().warn('AGV: Obstacle detected! Halting mission.')
            self._send_agv_command('stop')
        elif not msg.data and self.agv_obstacle:
            self.get_logger().info('AGV: Obstacle cleared.')
        self.agv_obstacle = msg.data

    def _arm_status_cb(self, msg: String):
        prev = self.arm_status
        self.arm_status  = msg.data
        self.last_arm_seen = time.time()
        self.get_logger().debug(f'Arm status: {msg.data}')

        # Arm finished picking → tell AGV to move
        if prev == 'PICKING' and msg.data == 'HOLDING':
            if self.mission_state == MissionState.ARM_PICKING:
                self.get_logger().info('Arm pick complete. Dispatching AGV.')
                self.mission_state = MissionState.AGV_MOVING
                self._send_agv_command('move_forward')

        # Arm finished placing → mission complete
        elif prev == 'PLACING' and msg.data == 'IDLE':
            if self.mission_state == MissionState.ARM_PLACING:
                self.get_logger().info('Mission complete!')
                self.mission_state = MissionState.COMPLETE

    def _server_rfid_cb(self, msg: String):
        """Called when server ESP32 scans a box RFID tag."""
        tag = msg.data.strip()
        self.get_logger().info(f'Server RFID scan: {tag}')

        if self.mission_state != MissionState.IDLE:
            self.get_logger().warn('Mission already in progress. Ignoring scan.')
            return

        # Determine target rack from box tag prefix
        rack = None
        for prefix, dest in BOX_RACK_MAP.items():
            if tag.startswith(prefix):
                rack = dest
                break

        if rack:
            self.current_box_tag = tag
            self.target_rack     = rack
            self.mission_state   = MissionState.BOX_DETECTED
            self.get_logger().info(
                f'Box detected: {tag} → target rack: {rack}')
        else:
            self.get_logger().warn(f'Unknown box tag prefix in: {tag}')

    def _server_inventory_cb(self, msg: String):
        try:
            inv = json.loads(msg.data)
            self.get_logger().debug(f'Inventory update: {inv}')
        except json.JSONDecodeError:
            pass

    # -----------------------------------------------------------------------
    # State Machine
    # -----------------------------------------------------------------------

    def _state_machine_tick(self):
        """Main mission state machine — runs every 500ms."""

        if self.mission_state == MissionState.BOX_DETECTED:
            # Command arm to pick
            self.get_logger().info('Commanding arm to pick box.')
            self._send_arm_command('pick')
            self.mission_state = MissionState.ARM_PICKING

        elif self.mission_state == MissionState.AGV_ARRIVED:
            # Command arm to place
            self.get_logger().info('Commanding arm to place box.')
            self._send_arm_command('place')
            self.mission_state = MissionState.ARM_PLACING

        elif self.mission_state == MissionState.COMPLETE:
            # Reset for next mission
            self.get_logger().info('Resetting for next mission.')
            self.current_box_tag = ''
            self.target_rack     = ''
            self.mission_state   = MissionState.IDLE
            self._send_agv_command('stop')
            self._send_arm_command('home')

        elif self.mission_state == MissionState.ERROR:
            # Safety: stop everything
            self._send_agv_command('stop')

    # -----------------------------------------------------------------------
    # Command Helpers
    # -----------------------------------------------------------------------

    def _send_agv_command(self, cmd: str):
        """Send a string command to the AGV."""
        msg = String()
        msg.data = cmd
        self.agv_command_pub.publish(msg)
        self.get_logger().info(f'→ AGV command: {cmd}')

    def _send_agv_twist(self, linear_x: float, angular_z: float):
        """Send a Twist velocity command (Nav2-compatible)."""
        twist = Twist()
        twist.linear.x  = linear_x
        twist.angular.z = angular_z
        self.agv_cmd_vel_pub.publish(twist)

    def _send_arm_command(self, cmd: str):
        """Send a string command to the Robotic Arm."""
        msg = String()
        msg.data = cmd
        self.arm_command_pub.publish(msg)
        self.get_logger().info(f'→ Arm command: {cmd}')

    def _rack_to_rfid_tag(self, rack: str) -> str:
        """Reverse-lookup: rack name → expected RFID floor tag."""
        for tag, dest in RFID_WAYPOINT_MAP.items():
            if dest == rack:
                return tag
        return ''

    # -----------------------------------------------------------------------
    # Diagnostics & Status
    # -----------------------------------------------------------------------

    def _publish_mission_status(self):
        now = time.time()
        status = {
            'mission':      self.mission_state,
            'box_tag':      self.current_box_tag,
            'target_rack':  self.target_rack,
            'agv_status':   self.agv_status,
            'agv_battery':  round(self.agv_battery, 1),
            'agv_location': self.agv_location,
            'agv_obstacle': self.agv_obstacle,
            'arm_status':   self.arm_status,
            'agv_online':   (now - self.last_agv_seen) < 10.0,
            'arm_online':   (now - self.last_arm_seen) < 10.0,
        }
        msg = String()
        msg.data = json.dumps(status)
        self.mission_status_pub.publish(msg)

    def _publish_diagnostics(self):
        now = time.time()
        array = DiagnosticArray()
        array.header.stamp = self.get_clock().now().to_msg()

        # AGV diagnostic
        agv_diag = DiagnosticStatus()
        agv_diag.name = 'AGV/LF-AGV-01'
        agv_diag.hardware_id = 'ESP32'
        agv_diag.values = [
            KeyValue(key='status',   value=self.agv_status),
            KeyValue(key='battery',  value=f'{self.agv_battery:.1f}%'),
            KeyValue(key='location', value=self.agv_location),
            KeyValue(key='obstacle', value=str(self.agv_obstacle)),
        ]
        agv_diag.level   = DiagnosticStatus.OK
        agv_diag.message = 'OK'
        if (now - self.last_agv_seen) > 10.0:
            agv_diag.level   = DiagnosticStatus.ERROR
            agv_diag.message = 'AGV offline (no heartbeat >10s)'
        elif self.agv_obstacle:
            agv_diag.level   = DiagnosticStatus.WARN
            agv_diag.message = 'Obstacle detected'
        elif self.agv_battery < 20.0:
            agv_diag.level   = DiagnosticStatus.WARN
            agv_diag.message = 'Low battery'

        # Arm diagnostic
        arm_diag = DiagnosticStatus()
        arm_diag.name        = 'Arm/Arm-01'
        arm_diag.hardware_id = 'ESP32'
        arm_diag.values = [
            KeyValue(key='status', value=self.arm_status),
        ]
        arm_diag.level   = DiagnosticStatus.OK
        arm_diag.message = 'OK'
        if (now - self.last_arm_seen) > 10.0:
            arm_diag.level   = DiagnosticStatus.ERROR
            arm_diag.message = 'Arm offline (no heartbeat >10s)'

        array.status = [agv_diag, arm_diag]
        self.diagnostics_pub.publish(array)


def main(args=None):
    rclpy.init(args=args)
    node = WarehouseManagerNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        node.get_logger().info('Shutting down WarehouseManagerNode.')
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
