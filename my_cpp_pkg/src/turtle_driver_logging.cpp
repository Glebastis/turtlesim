#include "rclcpp/rclcpp.hpp"
#include "turtlesim/msg/pose.hpp"
#include "turtlesim/srv/kill.hpp"
#include "geometry_msgs/msg/twist.hpp"
#include "my_robot_interfaces/msg/turtle_info.hpp"

#include <cmath>
#include <map>
#include <string>
#include <memory>
#include <algorithm>
#include <chrono>

// Prometheus
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>

class TurtleController : public rclcpp::Node {
public:
    TurtleController() : Node("turtle_controller"),
                         has_target_(false),
                         eat_radius_(0.5),
                         arrival_distance_(0.3),
                         linear_speed_(1.0),
                         max_angular_speed_(1.5),
                         angular_gain_(1.5),
                         total_eaten_(0),
                         target_switches_(0),
                         no_target_loops_(0) {
        
        // Initialize Prometheus
        initPrometheus();
        
        // Subscriptions
        pose_sub_ = this->create_subscription<turtlesim::msg::Pose>(
            "/turtle1/pose", 10,
            std::bind(&TurtleController::poseCallback, this, std::placeholders::_1));
        
        spawn_sub_ = this->create_subscription<my_robot_interfaces::msg::TurtleInfo>(
            "/spawned", 10,
            std::bind(&TurtleController::spawnCallback, this, std::placeholders::_1));
        
        removed_sub_ = this->create_subscription<my_robot_interfaces::msg::TurtleInfo>(
            "/removed", 10,
            std::bind(&TurtleController::removedCallback, this, std::placeholders::_1));
        
        // Publishers
        cmd_vel_pub_ = this->create_publisher<geometry_msgs::msg::Twist>("/turtle1/cmd_vel", 10);
        removed_pub_ = this->create_publisher<my_robot_interfaces::msg::TurtleInfo>("/removed", 10);
        
        // Kill client
        kill_client_ = this->create_client<turtlesim::srv::Kill>("/kill");
        
        // Control timer (10 Hz)
        control_timer_ = this->create_wall_timer(
            std::chrono::milliseconds(100),
            std::bind(&TurtleController::controlLoop, this));
        
        // Metrics publication timer (every 5 seconds)
        metrics_timer_ = this->create_wall_timer(
            std::chrono::seconds(5),
            std::bind(&TurtleController::publishMetrics, this));
        
        RCLCPP_INFO(this->get_logger(), "TurtleController started. Waiting for turtles...");
        RCLCPP_INFO(this->get_logger(), "Prometheus metrics available at http://0.0.0.0:9092/metrics");
    }

private:
    struct TurtleData {
        std::string name;
        double x;
        double y;
        double distance;
        std::chrono::time_point<std::chrono::steady_clock> spawn_time;
    };
    
    void initPrometheus() {
        try {
            exposer_ = std::make_unique<prometheus::Exposer>("0.0.0.0:9092");
            registry_ = std::make_shared<prometheus::Registry>();
            exposer_->RegisterCollectable(registry_);
            
            // Define buckets for time measurements
            std::vector<double> time_buckets = {0.1, 0.5, 1, 2, 5, 10, 30, 60, 120, 300};
            std::vector<double> distance_buckets = {0.1, 0.2, 0.5, 1, 2, 3, 4, 5, 7, 10};
            
            // Register metrics
            auto& active_gauge_family = prometheus::BuildGauge()
                .Name("turtle_controller_active_prey")
                .Help("Current number of prey turtles")
                .Register(*registry_);
            active_prey_gauge_ = &active_gauge_family.Add({});
            
            auto& eaten_counter_family = prometheus::BuildCounter()
                .Name("turtle_prey_eaten_total")
                .Help("Total number of prey eaten")
                .Register(*registry_);
            eaten_counter_ = &eaten_counter_family.Add({});
            
            auto& distance_gauge_family = prometheus::BuildGauge()
                .Name("turtle_distance_to_target_m")
                .Help("Current distance to target turtle")
                .Register(*registry_);
            distance_to_target_gauge_ = &distance_gauge_family.Add({});
            
            auto& switches_counter_family = prometheus::BuildCounter()
                .Name("turtle_target_switches_total")
                .Help("Total number of target switches")
                .Register(*registry_);
            switches_counter_ = &switches_counter_family.Add({});
            
            // Fix: Histogram requires bucket boundaries
            auto& time_to_eat_histogram_family = prometheus::BuildHistogram()
                .Name("turtle_time_to_eat_seconds")
                .Help("Time from prey spawn to being eaten")
                .Register(*registry_);
            time_to_eat_histogram_ = &time_to_eat_histogram_family.Add(
                {}, prometheus::Histogram::BucketBoundaries(time_buckets));
            
            auto& no_target_gauge_family = prometheus::BuildGauge()
                .Name("turtle_no_target_loops")
                .Help("Number of control loops with no target")
                .Register(*registry_);
            no_target_gauge_ = &no_target_gauge_family.Add({});
            
            // Fix: Distance histogram with bucket boundaries
            auto& distance_histogram_family = prometheus::BuildHistogram()
                .Name("turtle_distance_to_target_histogram")
                .Help("Histogram of distances to target")
                .Register(*registry_);
            distance_histogram_ = &distance_histogram_family.Add(
                {}, prometheus::Histogram::BucketBoundaries(distance_buckets));
            
            RCLCPP_INFO(this->get_logger(), "Prometheus initialized successfully");
        } catch (const std::exception& e) {
            RCLCPP_ERROR(this->get_logger(), "Failed to initialize Prometheus: %s", e.what());
        }
    }
    
    void publishMetrics() {
        if (!registry_) return;
        
        active_prey_gauge_->Set(turtles_.size());
        no_target_gauge_->Set(no_target_loops_);
        
        if (has_target_ && turtles_.count(current_target_)) {
            double dist = turtles_[current_target_].distance;
            distance_to_target_gauge_->Set(dist);
            distance_histogram_->Observe(dist);
        }
        
        RCLCPP_INFO_STREAM(this->get_logger(), 
            "METRICS: active_prey=" << turtles_.size() <<
            " total_eaten=" << total_eaten_ <<
            " target_switches=" << target_switches_ <<
            " no_target_loops=" << no_target_loops_ <<
            " has_target=" << (has_target_ ? "yes" : "no"));
    }
    
    void poseCallback(const turtlesim::msg::Pose::SharedPtr msg) {
        auto start_time = std::chrono::steady_clock::now();
        
        current_pose_ = *msg;
        updateAllDistances();
        
        if (has_target_ && !turtles_.empty()) {
            auto it = turtles_.find(current_target_);
            if (it != turtles_.end()) {
                // Check for closer turtle
                for (const auto& pair : turtles_) {
                    if (pair.first != current_target_ && pair.second.distance < it->second.distance - 0.1) {
                        RCLCPP_INFO(this->get_logger(), "Found closer turtle: %s (dist=%.2f) vs current target %s (dist=%.2f)",
                                    pair.first.c_str(), pair.second.distance,
                                    current_target_.c_str(), it->second.distance);
                        current_target_ = pair.first;
                        target_switches_++;
                        if (switches_counter_) switches_counter_->Increment();
                        break;
                    }
                }
            } else {
                has_target_ = false;
                selectNearestTarget();
            }
        } else if (!has_target_ && !turtles_.empty()) {
            selectNearestTarget();
        }
        
        double callback_time = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start_time).count();
        if (callback_time > 10.0) {
            RCLCPP_WARN_STREAM(this->get_logger(), 
                "Pose callback took " << callback_time << "ms (expected <10ms)");
        }
    }
    
    void spawnCallback(const my_robot_interfaces::msg::TurtleInfo::SharedPtr msg) {
        if (msg->action != "spawned") return;
        
        if (turtles_.find(msg->name) != turtles_.end()) {
            RCLCPP_WARN(this->get_logger(), "Duplicate spawn detected for %s, ignoring", msg->name.c_str());
            return;
        }
        
        TurtleData data;
        data.name = msg->name;
        data.x = msg->x;
        data.y = msg->y;
        data.distance = distanceTo(data);
        data.spawn_time = std::chrono::steady_clock::now();
        
        turtles_[msg->name] = data;
        
        RCLCPP_INFO(this->get_logger(), "New turtle spawned: %s at (%.2f, %.2f), dist=%.2f. Total turtles: %zu",
                    data.name.c_str(), data.x, data.y, data.distance, turtles_.size());
        
        if (!has_target_) {
            selectNearestTarget();
        } else {
            auto current_it = turtles_.find(current_target_);
            if (current_it != turtles_.end() && data.distance < current_it->second.distance - 0.1) {
                RCLCPP_INFO(this->get_logger(), "New turtle %s is closer than current target %s, switching!",
                            data.name.c_str(), current_target_.c_str());
                current_target_ = data.name;
                target_switches_++;
                if (switches_counter_) switches_counter_->Increment();
            }
        }
    }
    
    void removedCallback(const my_robot_interfaces::msg::TurtleInfo::SharedPtr msg) {
        if (msg->action != "removed") return;
        
        auto it = turtles_.find(msg->name);
        if (it != turtles_.end()) {
            RCLCPP_INFO(this->get_logger(), "External removal detected: %s", msg->name.c_str());
            
            // Calculate lifetime if we have spawn time
            auto now = std::chrono::steady_clock::now();
            double lifetime = std::chrono::duration<double>(now - it->second.spawn_time).count();
            RCLCPP_INFO_STREAM(this->get_logger(), 
                "Turtle " << msg->name << " lived for " << lifetime << " seconds before removal");
            
            turtles_.erase(it);
            
            if (current_target_ == msg->name) {
                current_target_.clear();
                has_target_ = false;
                selectNearestTarget();
            }
        }
    }
    
    void updateAllDistances() {
        for (auto& pair : turtles_) {
            pair.second.distance = distanceTo(pair.second);
        }
    }
    
    double distanceTo(const TurtleData& other) {
        double dx = other.x - current_pose_.x;
        double dy = other.y - current_pose_.y;
        return std::hypot(dx, dy);
    }
    
    void selectNearestTarget() {
        if (turtles_.empty()) {
            has_target_ = false;
            current_target_.clear();
            RCLCPP_INFO(this->get_logger(), "No turtles left, switching to circular motion");
            return;
        }
        
        auto nearest = std::min_element(turtles_.begin(), turtles_.end(),
            [](const auto& a, const auto& b) {
                return a.second.distance < b.second.distance;
            });
        
        current_target_ = nearest->first;
        has_target_ = true;
        RCLCPP_INFO(this->get_logger(), "Nearest target: %s (dist=%.2f). Total turtles in list: %zu",
                    current_target_.c_str(), nearest->second.distance, turtles_.size());
    }
    
    void killTurtle(const std::string& name) {
        RCLCPP_INFO(this->get_logger(), "Attempting to kill turtle: %s", name.c_str());
        
        auto request = std::make_shared<turtlesim::srv::Kill::Request>();
        request->name = name;
        
        auto start_time = std::chrono::steady_clock::now();
        
        auto future = kill_client_->async_send_request(
            request,
            [this, name, start_time](rclcpp::Client<turtlesim::srv::Kill>::SharedFuture future) {
                auto end_time = std::chrono::steady_clock::now();
                double latency_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
                
                try {
                    future.get();
                    total_eaten_++;
                    if (eaten_counter_) eaten_counter_->Increment();
                    
                    // Calculate time to eat
                    auto it = turtles_.find(name);
                    if (it != turtles_.end()) {
                        double time_to_eat = std::chrono::duration<double>(end_time - it->second.spawn_time).count();
                        if (time_to_eat_histogram_) time_to_eat_histogram_->Observe(time_to_eat);
                        
                        RCLCPP_INFO_STREAM(this->get_logger(), 
                            "Successfully killed turtle: " << name << 
                            " (time_to_eat=" << time_to_eat << "s, kill_latency=" << latency_ms << "ms)");
                    } else {
                        RCLCPP_INFO_STREAM(this->get_logger(), 
                            "Successfully killed turtle: " << name << 
                            " (kill_latency=" << latency_ms << "ms)");
                    }
                    
                    // Publish removal message
                    auto remove_msg = my_robot_interfaces::msg::TurtleInfo();
                    remove_msg.name = name;
                    remove_msg.x = 0.0;
                    remove_msg.y = 0.0;
                    remove_msg.theta = 0.0;
                    remove_msg.action = "removed";
                    remove_msg.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    removed_pub_->publish(remove_msg);
                    
                    // Remove from list
                    turtles_.erase(name);
                    RCLCPP_INFO(this->get_logger(), "Removed from list. Remaining turtles: %zu", turtles_.size());
                    
                    if (current_target_ == name) {
                        current_target_.clear();
                        has_target_ = false;
                        if (!turtles_.empty()) {
                            RCLCPP_INFO(this->get_logger(), "Selecting next target from %zu remaining turtles", turtles_.size());
                            selectNearestTarget();
                        } else {
                            RCLCPP_INFO(this->get_logger(), "No turtles remaining, will circle");
                        }
                    }
                } catch (const std::exception& e) {
                    RCLCPP_ERROR_STREAM(this->get_logger(), 
                        "Failed to kill turtle " << name << ": " << e.what() << " (latency=" << latency_ms << "ms)");
                }
            });
    }
    
    void checkAndEat() {
        if (!has_target_) return;
        
        auto it = turtles_.find(current_target_);
        if (it == turtles_.end()) {
            has_target_ = false;
            selectNearestTarget();
            return;
        }
        
        double dist = it->second.distance;
        if (dist < eat_radius_) {
            RCLCPP_INFO(this->get_logger(), "Ate turtle %s at distance %.2f (radius=%.2f)", 
                        current_target_.c_str(), dist, eat_radius_);
            killTurtle(current_target_);
        }
    }
    
    void controlLoop() {
        auto loop_start = std::chrono::steady_clock::now();
        
        checkAndEat();
        
        if (!has_target_ || turtles_.empty()) {
            no_target_loops_++;
            // Circular motion
            geometry_msgs::msg::Twist cmd;
            cmd.linear.x = linear_speed_;
            cmd.angular.z = 0.8;
            cmd_vel_pub_->publish(cmd);
        } else {
            // Movement to target
            auto it = turtles_.find(current_target_);
            if (it == turtles_.end()) {
                has_target_ = false;
                selectNearestTarget();
                return;
            }
            
            auto& target = it->second;
            double dx = target.x - current_pose_.x;
            double dy = target.y - current_pose_.y;
            double target_angle = std::atan2(dy, dx);
            
            double angle_diff = target_angle - current_pose_.theta;
            while (angle_diff > M_PI) angle_diff -= 2 * M_PI;
            while (angle_diff < -M_PI) angle_diff += 2 * M_PI;
            
            geometry_msgs::msg::Twist cmd;
            
            if (std::abs(angle_diff) < 0.2) {
                cmd.linear.x = linear_speed_;
                cmd.angular.z = angular_gain_ * angle_diff;
                cmd.angular.z = std::clamp(cmd.angular.z, -max_angular_speed_, max_angular_speed_);
            } else {
                cmd.linear.x = 0.0;
                cmd.angular.z = angular_gain_ * angle_diff;
                cmd.angular.z = std::clamp(cmd.angular.z, -max_angular_speed_, max_angular_speed_);
            }
            
            cmd_vel_pub_->publish(cmd);
        }
        
        double loop_time = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - loop_start).count();
        if (loop_time > 15.0) {
            RCLCPP_WARN_STREAM(this->get_logger(), 
                "Control loop took " << loop_time << "ms (expected ~10ms)");
        }
    }
    
    // ROS2 Communication
    rclcpp::Subscription<turtlesim::msg::Pose>::SharedPtr pose_sub_;
    rclcpp::Subscription<my_robot_interfaces::msg::TurtleInfo>::SharedPtr spawn_sub_;
    rclcpp::Subscription<my_robot_interfaces::msg::TurtleInfo>::SharedPtr removed_sub_;
    rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_pub_;
    rclcpp::Publisher<my_robot_interfaces::msg::TurtleInfo>::SharedPtr removed_pub_;
    rclcpp::Client<turtlesim::srv::Kill>::SharedPtr kill_client_;
    rclcpp::TimerBase::SharedPtr control_timer_;
    rclcpp::TimerBase::SharedPtr metrics_timer_;
    
    // State
    turtlesim::msg::Pose current_pose_;
    std::map<std::string, TurtleData> turtles_;
    std::string current_target_;
    bool has_target_;
    
    // Parameters
    const double eat_radius_;
    const double arrival_distance_;
    const double linear_speed_;
    const double max_angular_speed_;
    const double angular_gain_;
    
    // Statistics
    int total_eaten_;
    int target_switches_;
    int no_target_loops_;
    
    // Prometheus
    std::unique_ptr<prometheus::Exposer> exposer_;
    std::shared_ptr<prometheus::Registry> registry_;
    prometheus::Gauge* active_prey_gauge_;
    prometheus::Counter* eaten_counter_;
    prometheus::Gauge* distance_to_target_gauge_;
    prometheus::Counter* switches_counter_;
    prometheus::Histogram* time_to_eat_histogram_;
    prometheus::Gauge* no_target_gauge_;
    prometheus::Histogram* distance_histogram_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<TurtleController>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}