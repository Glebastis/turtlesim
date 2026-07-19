from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import ExecuteProcess
import os

def generate_launch_description():
    return LaunchDescription([
        Node(
            package='turtlesim',
            executable='turtlesim_node',
            name='turtlesim_node',
            parameters=[{
                'background_r': 0,
                'background_g': 0,
                'background_b': 0,
                'use_sim_time': False
            }],
            output='screen',
            emulate_tty=True,
        ),
        Node(
            package='my_cpp_pkg',
            executable='turtle_manager_logging',
            name='turtle_manager',
            parameters=[{
                'target_turtle_count': 5,
                'spawn_interval_seconds': 1,
                'auto_replace_enabled': True
            }],
            output='screen',
            emulate_tty=True,
            prefix=['stdbuf -oL -eL'],  # Line buffering for logs
        ),
        Node(
            package='my_cpp_pkg',
            executable='turtle_driver_logging',
            name='turtle_driver',
            output='screen',
            emulate_tty=True,
            prefix=['stdbuf -oL -eL'],
        ),
    ])