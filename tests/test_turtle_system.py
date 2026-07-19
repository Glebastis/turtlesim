#!/usr/bin/env python3
import pytest
import rclpy
from rclpy.node import Node
from geometry_msgs.msg import Twist
from turtlesim.srv import Spawn, Kill
from my_robot_interfaces.srv import RemoveTurtle
from my_robot_interfaces.msg import TurtleInfo
import my_robot_interfaces
import turtlesim
import time
import math

@pytest.fixture(scope="module")
def ros_node():
    rclpy.init()
    node = Node('test_harness')
    yield node
    node.destroy_node()
    rclpy.shutdown()

def test_spawn_turtle(ros_node):
    """Тест спавна черепашки"""
    client = ros_node.create_client(Spawn, '/spawn')
    
    # Wait for service
    assert client.wait_for_service(timeout_sec=5.0)
    
    # Create request
    request = Spawn.Request()
    request.x = 5.0
    request.y = 5.0
    request.theta = 0.0
    request.name = "test_turtle"
    
    # Call service
    future = client.call_async(request)
    rclpy.spin_until_future_complete(ros_node, future)
    
    assert future.result() is not None
    assert future.result().name == "test_turtle"

def test_kill_turtle(ros_node):
    """Тест удаления черепашки"""
    # First spawn a turtle
    spawn_client = ros_node.create_client(Spawn, '/spawn')
    spawn_client.wait_for_service(timeout_sec=5.0)
    
    spawn_req = Spawn.Request()
    spawn_req.name = "turtle_to_kill"
    spawn_req.x = 3.0
    spawn_req.y = 3.0
    
    spawn_future = spawn_client.call_async(spawn_req)
    rclpy.spin_until_future_complete(ros_node, spawn_future)
    assert spawn_future.result() is not None
    
    # Kill the turtle
    kill_client = ros_node.create_client(Kill, '/kill')
    kill_client.wait_for_service(timeout_sec=5.0)
    
    kill_req = Kill.Request()
    kill_req.name = "turtle_to_kill"
    
    kill_future = kill_client.call_async(kill_req)
    rclpy.spin_until_future_complete(ros_node, kill_future)
    
    assert kill_future.result() is not None

def test_remove_turtle_service(ros_node):
    """Тест сервиса удаления"""
    # Spawn test turtle
    spawn_client = ros_node.create_client(Spawn, '/spawn')
    spawn_client.wait_for_service(timeout_sec=5.0)
    
    spawn_req = Spawn.Request()
    spawn_req.name = "service_turtle"
    spawn_req.x = 7.0
    spawn_req.y = 7.0
    
    spawn_future = spawn_client.call_async(spawn_req)
    rclpy.spin_until_future_complete(ros_node, spawn_future)
    assert spawn_future.result() is not None
    
    # Call remove service
    remove_client = ros_node.create_client(RemoveTurtle, '/remove_turtle')
    remove_client.wait_for_service(timeout_sec=5.0)
    
    remove_req = RemoveTurtle.Request()
    remove_req.name = "service_turtle"
    
    remove_future = remove_client.call_async(remove_req)
    rclpy.spin_until_future_complete(ros_node, remove_future)
    
    assert remove_future.result() is not None
    assert remove_future.result().success == True

def test_turtle_movement(ros_node):
    """Тест движения черепашки"""
    pub = ros_node.create_publisher(Twist, '/turtle1/cmd_vel', 10)
    
    # Create subscriber to monitor pose
    pose_received = False
    
    def pose_callback(msg):
        nonlocal pose_received
        pose_received = True
    
    sub = ros_node.create_subscription(turtlesim.msg.Pose, '/turtle1/pose', pose_callback, 10)
    
    # Send movement command
    cmd = Twist()
    cmd.linear.x = 1.0
    cmd.angular.z = 0.0
    
    for _ in range(10):
        pub.publish(cmd)
        rclpy.spin_once(ros_node, timeout_sec=0.1)
    
    assert pose_received

def test_metrics_endpoint():
    """Тест endpoint метрик Prometheus"""
    import requests
    import time
    
    time.sleep(2)  # Wait for metrics to be published
    
    response = requests.get('http://localhost:9091/metrics')
    assert response.status_code == 200
    
    metrics_text = response.text
    assert 'turtle_active_count' in metrics_text
    assert 'turtle_spawns_total' in metrics_text

@pytest.mark.slow
def test_system_stability(ros_node):
    """Тест стабильности системы"""
    duration = 30  # seconds
    start_time = time.time()
    error_count = 0
    
    error_sub = ros_node.create_subscription(
        my_robot_interfaces.msg.TurtleInfo,
        '/spawned',
        lambda msg: None,
        10
    )
    
    while time.time() - start_time < duration:
        try:
            rclpy.spin_once(ros_node, timeout_sec=0.1)
        except Exception as e:
            error_count += 1
            print(f"Error: {e}")
    
    assert error_count < 5  # Allow small number of errors

if __name__ == '__main__':
    pytest.main([__file__, '-v', '--tb=short'])