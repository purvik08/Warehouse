#!/usr/bin/env python3
"""
fleet_monitor_node.py
=====================
Real-time fleet telemetry monitor for the Warehouse Automation System.
Runs on Raspberry Pi 5 alongside the warehouse_manager_node.

Subscribes to all robot telemetry topics and prints a live terminal
dashboard. Also logs all events to ~/warehouse_logs/ for post-analysis.

Usage:
  ros2 run warehouse_ros2 fleet_monitor
"""

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy
import json
import os
import time
from datetime import datetime

from std_msgs.msg import String, Bool
from sensor_msgs.msg import BatteryState
from diagnostic_msgs.msg import DiagnosticArray


# ANSI colour codes for terminal dashboard
RED    = '\033[91m'
GREEN  = '\033[92m'
YELLOW = '\033[93m'
CYAN   = '\033[96m'
WHITE  = '\033[97m'
BOLD   = '\033[1m'
RESET  = '\033[0m'
CLEAR  = '\033[2J\033[H'


class FleetMonitorNode(Node):
    """Subscribes to all fleet topics and renders a live terminal dashboard."""

    def __init__(self):
        super().__init__('fleet_monitor')

        reliable_qos = QoSProfile(
            reliability=ReliabilityPolicy.RELIABLE,
            depth=10
        )
        sensor_qos = QoSProfile(
            reliability=ReliabilityPolicy.BEST_EFFORT,
            depth=5
        )

        # --- Fleet state ---------------------------------------------------
        self.fleet = {
            'AGV': {
                'status':   'OFFLINE',
                'battery':  0.0,
                'location': 'N/A',
                'obstacle': False,
                'last_seen': 0.0,
            },
            'ARM': {
                'status':   'OFFLINE',
                'battery':  0.0,
                'last_seen': 0.0,
            },
            'SERVER': {
                'last_rfid':    'N/A',
                'inventory':    {},
                'last_seen':    0.0,
            },
        }
        self.mission_state = {}
        self.event_log = []

        # --- Log directory setup -------------------------------------------
        log_dir = os.path.expanduser('~/warehouse_logs')
        os.makedirs(log_dir, exist_ok=True)
        ts = datetime.now().strftime('%Y%m%d_%H%M%S')
        self.log_file = open(os.path.join(log_dir, f'fleet_{ts}.csv'), 'w')
        self.log_file.write('timestamp,source,event,data\n')
        self.get_logger().info(f'Logging to: {self.log_file.name}')

        # --- Subscriptions -------------------------------------------------
        self.create_subscription(
            String, '/warehouse/agv/status', self._agv_status_cb, reliable_qos)
        self.create_subscription(
            BatteryState, '/warehouse/agv/battery', self._agv_battery_cb, sensor_qos)
        self.create_subscription(
            String, '/warehouse/agv/location', self._agv_location_cb, reliable_qos)
        self.create_subscription(
            Bool, '/warehouse/agv/obstacle', self._agv_obstacle_cb, sensor_qos)
        self.create_subscription(
            String, '/warehouse/arm/status', self._arm_status_cb, reliable_qos)
        self.create_subscription(
            String, '/warehouse/server/rfid', self._rfid_cb, reliable_qos)
        self.create_subscription(
            String, '/warehouse/server/inventory', self._inventory_cb, reliable_qos)
        self.create_subscription(
            String, '/warehouse/mission/status', self._mission_cb, reliable_qos)

        # Refresh display every 1s
        self.create_timer(1.0, self._render_dashboard)

    # -----------------------------------------------------------------------
    # Callbacks
    # -----------------------------------------------------------------------

    def _agv_status_cb(self, msg: String):
        self.fleet['AGV']['status']    = msg.data
        self.fleet['AGV']['last_seen'] = time.time()
        self._log('AGV', 'status', msg.data)

    def _agv_battery_cb(self, msg: BatteryState):
        self.fleet['AGV']['battery'] = msg.percentage * 100.0

    def _agv_location_cb(self, msg: String):
        self.fleet['AGV']['location']  = msg.data
        self.fleet['AGV']['last_seen'] = time.time()
        self._log('AGV', 'location', msg.data)
        self._add_event(f'AGV location: {msg.data}')

    def _agv_obstacle_cb(self, msg: Bool):
        prev = self.fleet['AGV']['obstacle']
        self.fleet['AGV']['obstacle'] = msg.data
        if msg.data and not prev:
            self._log('AGV', 'obstacle', 'DETECTED')
            self._add_event(f'{RED}⚠ OBSTACLE DETECTED{RESET}')
        elif not msg.data and prev:
            self._log('AGV', 'obstacle', 'CLEARED')
            self._add_event(f'{GREEN}✓ Obstacle cleared{RESET}')

    def _arm_status_cb(self, msg: String):
        self.fleet['ARM']['status']    = msg.data
        self.fleet['ARM']['last_seen'] = time.time()
        self._log('ARM', 'status', msg.data)

    def _rfid_cb(self, msg: String):
        self.fleet['SERVER']['last_rfid'] = msg.data
        self.fleet['SERVER']['last_seen'] = time.time()
        self._log('SERVER', 'rfid_scan', msg.data)
        self._add_event(f'RFID scan: {msg.data}')

    def _inventory_cb(self, msg: String):
        try:
            self.fleet['SERVER']['inventory'] = json.loads(msg.data)
        except json.JSONDecodeError:
            pass

    def _mission_cb(self, msg: String):
        try:
            self.mission_state = json.loads(msg.data)
        except json.JSONDecodeError:
            pass

    # -----------------------------------------------------------------------
    # Helpers
    # -----------------------------------------------------------------------

    def _log(self, source: str, event: str, data: str):
        ts = datetime.now().isoformat()
        self.log_file.write(f'{ts},{source},{event},{data}\n')
        self.log_file.flush()

    def _add_event(self, msg: str):
        ts = datetime.now().strftime('%H:%M:%S')
        self.event_log.append(f'[{ts}] {msg}')
        if len(self.event_log) > 12:
            self.event_log.pop(0)

    def _status_colour(self, status: str, last_seen: float) -> str:
        now = time.time()
        if (now - last_seen) > 10.0:
            return f'{RED}OFFLINE{RESET}'
        if status in ('MOVING', 'TURNING', 'PICKING', 'PLACING'):
            return f'{YELLOW}{status}{RESET}'
        if status in ('IDLE', 'HOLDING'):
            return f'{GREEN}{status}{RESET}'
        return f'{WHITE}{status}{RESET}'

    def _battery_bar(self, pct: float) -> str:
        filled = int(pct / 10)
        bar    = '█' * filled + '░' * (10 - filled)
        colour = GREEN if pct > 50 else YELLOW if pct > 20 else RED
        return f'{colour}[{bar}] {pct:.0f}%{RESET}'

    # -----------------------------------------------------------------------
    # Terminal Dashboard
    # -----------------------------------------------------------------------

    def _render_dashboard(self):
        now = time.time()
        agv = self.fleet['AGV']
        arm = self.fleet['ARM']
        srv = self.fleet['SERVER']
        inv = srv.get('inventory', {})

        lines = [
            f'{BOLD}{CYAN}╔══════════════════════════════════════════════════════╗{RESET}',
            f'{BOLD}{CYAN}║    🏭  Warehouse Automation Fleet Monitor (ROS2)     ║{RESET}',
            f'{BOLD}{CYAN}╚══════════════════════════════════════════════════════╝{RESET}',
            '',
            f'{BOLD}── AGV (LF-AGV-01) ──────────────────────────────────{RESET}',
            f'  Status   : {self._status_colour(agv["status"], agv["last_seen"])}',
            f'  Battery  : {self._battery_bar(agv["battery"])}',
            f'  Location : {CYAN}{agv["location"]}{RESET}',
            f'  Obstacle : {RED + "⚠ YES" + RESET if agv["obstacle"] else GREEN + "Clear" + RESET}',
            '',
            f'{BOLD}── Robotic Arm (Arm-01) ─────────────────────────────{RESET}',
            f'  Status   : {self._status_colour(arm["status"], arm["last_seen"])}',
            '',
            f'{BOLD}── Inventory ────────────────────────────────────────{RESET}',
            f'  Rack A   : {CYAN}{inv.get("rackA", "?")}{RESET} / 500',
            f'  Rack B   : {CYAN}{inv.get("rackB", "?")}{RESET} / 600',
            f'  Rack C   : {CYAN}{inv.get("rackC", "?")}{RESET} / 700',
            f'  Last Tag : {YELLOW}{srv["last_rfid"]}{RESET}',
            '',
            f'{BOLD}── Mission ───────────────────────────────────────────{RESET}',
            f'  State    : {BOLD}{YELLOW}{self.mission_state.get("mission", "N/A")}{RESET}',
            f'  Box      : {self.mission_state.get("box_tag", "none")}',
            f'  Target   : {self.mission_state.get("target_rack", "none")}',
            '',
            f'{BOLD}── Recent Events ─────────────────────────────────────{RESET}',
        ]

        for ev in self.event_log[-8:]:
            lines.append(f'  {ev}')

        lines.append('')
        lines.append(f'  {WHITE}Updated: {datetime.now().strftime("%H:%M:%S")}{RESET}')

        print(CLEAR + '\n'.join(lines), flush=True)

    def destroy_node(self):
        if self.log_file:
            self.log_file.close()
        super().destroy_node()


def main(args=None):
    rclpy.init(args=args)
    node = FleetMonitorNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        print('\nShutting down fleet monitor.')
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
