from setuptools import find_packages, setup
import os
from glob import glob

package_name = 'warehouse_ros2'

setup(
    name=package_name,
    version='1.0.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages',
            ['resource/' + package_name]),
        ('share/' + package_name, ['package.xml']),
        # Install launch files
        (os.path.join('share', package_name, 'launch'),
            glob(os.path.join('launch', '*.py'))),
        # Install config files
        (os.path.join('share', package_name, 'config'),
            glob(os.path.join('config', '*.yaml'))),
    ],
    install_requires=['setuptools'],
    zip_safe=True,
    maintainer='Warehouse Team',
    maintainer_email='warehouse@example.com',
    description='ROS2 Humble package for Warehouse Automation System on Raspberry Pi 5',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            # Main orchestrator — manages AGV missions and arm sequencing
            'warehouse_manager = warehouse_ros2.warehouse_manager_node:main',
            # Fleet telemetry monitor — subscribes to all robot topics
            'fleet_monitor    = warehouse_ros2.fleet_monitor_node:main',
            # Nav2 waypoint bridge — sends NavigateToPose goals from RFID map
            'nav2_bridge      = warehouse_ros2.nav2_bridge_node:main',
        ],
    },
)
