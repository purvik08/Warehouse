#!/usr/bin/env bash
# =============================================================================
# install_ros2_humble.sh
# Full setup script for the Warehouse Automation ROS2 stack on Raspberry Pi 5
# Ubuntu 22.04 (Jammy) — ROS2 Humble Hawksbill
#
# Run as a regular user (not root). Will use sudo where needed.
# Usage:
#   chmod +x install_ros2_humble.sh
#   ./install_ros2_humble.sh
# =============================================================================

set -euo pipefail

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

info "=== Warehouse Automation System — ROS2 Humble Install ==="
info "Platform: Raspberry Pi 5 / Ubuntu 22.04"

# ── 0. Sanity checks ──────────────────────────────────────────────────────
[[ "$(lsb_release -cs)" == "jammy" ]] || \
  error "This script requires Ubuntu 22.04 Jammy. Got: $(lsb_release -cs)"

# ── 1. System update ──────────────────────────────────────────────────────
info "Updating system packages..."
sudo apt-get update && sudo apt-get upgrade -y

# ── 2. ROS2 Humble base install ───────────────────────────────────────────
info "Installing ROS2 Humble..."
sudo apt-get install -y software-properties-common curl gnupg lsb-release

# Add ROS2 apt repository
sudo curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key \
  -o /usr/share/keyrings/ros-archive-keyring.gpg

echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] \
  http://packages.ros.org/ros2/ubuntu $(. /etc/os-release && echo $UBUNTU_CODENAME) main" | \
  sudo tee /etc/apt/sources.list.d/ros2.list > /dev/null

sudo apt-get update

# Install ROS2 Humble desktop (includes rviz2, rqt)
sudo apt-get install -y \
  ros-humble-desktop \
  ros-humble-ros-base \
  ros-humble-ros-dev-tools \
  python3-colcon-common-extensions \
  python3-rosdep \
  python3-pip

# ── 3. Nav2 packages ──────────────────────────────────────────────────────
info "Installing Nav2 stack..."
sudo apt-get install -y \
  ros-humble-navigation2 \
  ros-humble-nav2-bringup \
  ros-humble-nav2-msgs \
  ros-humble-slam-toolbox \
  ros-humble-tf2-ros \
  ros-humble-tf2-tools \
  ros-humble-robot-localization

# ── 4. micro-ROS Agent ───────────────────────────────────────────────────
info "Installing micro-ROS Agent..."

# Option A: Build from source (recommended for RPi5)
mkdir -p ~/microros_ws/src
cd ~/microros_ws

# Clone micro-ROS agent
git clone -b humble https://github.com/micro-ROS/micro_ros_setup.git src/micro_ros_setup

source /opt/ros/humble/setup.bash
rosdep update
rosdep install --from-paths src --ignore-src -y

colcon build --packages-select micro_ros_setup
source install/local_setup.bash

# Create micro-ROS agent package
ros2 run micro_ros_setup create_agent_ws.sh
ros2 run micro_ros_setup build_agent.sh
source install/local_setup.bash

info "micro-ROS Agent built at: ~/microros_ws/install/micro_ros_agent/lib/micro_ros_agent/"

cd ~

# ── 5. Warehouse ROS2 Package ─────────────────────────────────────────────
info "Setting up warehouse_ros2 workspace..."

WAREHOUSE_WS=~/warehouse_ws
mkdir -p "${WAREHOUSE_WS}/src"

# Detect script location and copy warehouse_ros2 package
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_SRC="${SCRIPT_DIR}/../"   # parent dir contains warehouse_ros2/

if [ -d "${PACKAGE_SRC}/warehouse_ros2" ]; then
  cp -r "${PACKAGE_SRC}/warehouse_ros2" "${WAREHOUSE_WS}/src/"
  info "Copied warehouse_ros2 package to workspace."
else
  warn "Could not find warehouse_ros2/ package. Copy it manually to ${WAREHOUSE_WS}/src/"
fi

# Build the workspace
cd "${WAREHOUSE_WS}"
source /opt/ros/humble/setup.bash
rosdep install --from-paths src --ignore-src -y
colcon build --symlink-install

info "warehouse_ros2 package built successfully."

# ── 6. Setup ~/.bashrc ───────────────────────────────────────────────────
info "Adding ROS2 source commands to ~/.bashrc..."

BASHRC_ADDITIONS="
# ── ROS2 Humble (Warehouse Automation System) ──
source /opt/ros/humble/setup.bash
source ~/microros_ws/install/local_setup.bash
source ~/warehouse_ws/install/local_setup.bash

# Set ROS domain ID (change if multiple ROS2 systems on same network)
export ROS_DOMAIN_ID=42

# micro-ROS Agent shortcut
alias microros-agent='~/microros_ws/install/micro_ros_agent/lib/micro_ros_agent/micro_ros_agent udp4 --port 8888 -v4'

# Warehouse shortcuts
alias warehouse-launch='ros2 launch warehouse_ros2 warehouse.launch.py'
alias warehouse-monitor='ros2 run warehouse_ros2 fleet_monitor'
alias rqt-graph='ros2 run rqt_graph rqt_graph'
"

if ! grep -q "ROS2 Humble (Warehouse" ~/.bashrc; then
  echo "${BASHRC_ADDITIONS}" >> ~/.bashrc
  info "Added ROS2 setup to ~/.bashrc"
else
  warn "ROS2 setup already in ~/.bashrc — skipping."
fi

# ── 7. rosdep init ───────────────────────────────────────────────────────
if [ ! -f /etc/ros/rosdep/sources.list.d/20-default.list ]; then
  sudo rosdep init
fi
rosdep update

# ── 8. Create systemd service for auto-start ─────────────────────────────
info "Creating systemd service for warehouse system auto-start..."

SERVICE_FILE="/etc/systemd/system/warehouse-ros2.service"
sudo tee "${SERVICE_FILE}" > /dev/null <<EOF
[Unit]
Description=Warehouse Automation ROS2 System
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${USER}
WorkingDirectory=${HOME}
Environment="ROS_DOMAIN_ID=42"
ExecStartPre=/bin/bash -c 'source /opt/ros/humble/setup.bash && source ${HOME}/microros_ws/install/local_setup.bash && source ${HOME}/warehouse_ws/install/local_setup.bash'
ExecStart=/bin/bash -c 'source /opt/ros/humble/setup.bash && source ${HOME}/microros_ws/install/local_setup.bash && source ${HOME}/warehouse_ws/install/local_setup.bash && ros2 launch warehouse_ros2 warehouse.launch.py'
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable warehouse-ros2.service
info "Systemd service installed. Enable with: sudo systemctl start warehouse-ros2"

# ── 9. Final instructions ─────────────────────────────────────────────────
echo ""
echo -e "${GREEN}══════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  ✅  Installation Complete!${NC}"
echo -e "${GREEN}══════════════════════════════════════════════════════════${NC}"
echo ""
echo "  Next steps:"
echo "  1. Reload bashrc:    source ~/.bashrc"
echo "  2. Flash ESP32 firmwares with micro-ROS (see README_ROS2.md)"
echo "  3. Set AGENT_IP in each ESP32 firmware to this RPi5's IP:"
echo "     $(hostname -I | awk '{print $1}')"
echo "  4. Launch the system: warehouse-launch"
echo "     Or manually:       ros2 launch warehouse_ros2 warehouse.launch.py"
echo ""
echo "  Useful commands:"
echo "    ros2 topic list          — list all active topics"
echo "    ros2 topic echo /warehouse/agv/status"
echo "    ros2 node list           — list running nodes"
echo "    rqt-graph                — visualise node/topic graph"
echo "    microros-agent           — start micro-ROS UDP agent separately"
echo ""
