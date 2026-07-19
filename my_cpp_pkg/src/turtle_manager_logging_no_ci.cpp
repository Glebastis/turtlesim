#include "rclcpp/rclcpp.hpp"
#include "turtlesim/srv/spawn.hpp"
#include "turtlesim/srv/kill.hpp"
#include "my_robot_interfaces/msg/turtle_info.hpp"
#include "my_robot_interfaces/srv/remove_turtle.hpp"

#include <random>
#include <chrono>
#include <set>
#include <string>
#include <memory>
#include <queue>
#include <map>
#include <deque>

// Prometheus
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>

class TurtleManager : public rclcpp::Node {
public:
    TurtleManager() : Node("turtle_manager"), 
                      target_turtle_count_(5),
                      spawn_interval_seconds_(1),
                      auto_replace_enabled_(true),
                      pending_spawns_(0),
                      is_spawning_(false),
                      total_spawned_(0),
                      total_removed_(0),
                      failed_spawns_(0),
                      failed_kills_(0) {
                          
        this->declare_parameter<int>("target_turtle_count", 5);
        this->declare_parameter<int>("spawn_interval_seconds", 1);
        this->declare_parameter<bool>("auto_replace_enabled", true);
        
        target_turtle_count_ = this->get_parameter("target_turtle_count").as_int();
        spawn_interval_seconds_ = this->get_parameter("spawn_interval_seconds").as_int();
        auto_replace_enabled_ = this->get_parameter("auto_replace_enabled").as_bool();
        
        // Initialize Prometheus
        initPrometheus();
        
        // Clients for turtlesim
        spawn_client_ = this->create_client<turtlesim::srv::Spawn>("/spawn");
        kill_client_ = this->create_client<turtlesim::srv::Kill>("/kill");
        
        // Service for removing turtles
        remove_service_ = this->create_service<my_robot_interfaces::srv::RemoveTurtle>(
            "/remove_turtle",
            std::bind(&TurtleManager::handleRemoveTurtle, this,
                     std::placeholders::_1, std::placeholders::_2));
        
        // Publishers for events
        spawned_pub_ = this->create_publisher<my_robot_interfaces::msg::TurtleInfo>("/spawned", 10);
        removed_pub_ = this->create_publisher<my_robot_interfaces::msg::TurtleInfo>("/removed", 10);
        
        // Subscription to removals
        removed_sub_ = this->create_subscription<my_robot_interfaces::msg::TurtleInfo>(
            "/removed", 10,
            std::bind(&TurtleManager::removedCallback, this, std::placeholders::_1));
        
        // Spawn timer
        spawn_timer_ = this->create_wall_timer(
            std::chrono::seconds(spawn_interval_seconds_),
            std::bind(&TurtleManager::maintainTurtleCount, this));
        
        // Metrics publication timer (every 5 seconds)
        metrics_timer_ = this->create_wall_timer(
            std::chrono::seconds(5),
            std::bind(&TurtleManager::publishMetrics, this));
        
        // Wait for turtlesim service
        while (!spawn_client_->wait_for_service(std::chrono::seconds(1))) {
            RCLCPP_WARN(this->get_logger(), "Waiting for turtlesim spawn service...");
            if (!rclcpp::ok()) {
                RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for service");
                return;
            }
        }
        
        // Initial spawn
        maintainTurtleCount();
        
        RCLCPP_INFO(this->get_logger(), "TurtleManager initialized. Target: %ld turtles", 
                   target_turtle_count_);
        RCLCPP_INFO(this->get_logger(), "Spawn interval: %d seconds", spawn_interval_seconds_);
        RCLCPP_INFO(this->get_logger(), "Auto-replace: %s", 
                   auto_replace_enabled_ ? "ENABLED" : "DISABLED");
        RCLCPP_INFO(this->get_logger(), "Prometheus metrics available at http://0.0.0.0:9091/metrics");
    }

private:
    void initPrometheus() {
        try {
            exposer_ = std::make_unique<prometheus::Exposer>("0.0.0.0:9091");
            registry_ = std::make_shared<prometheus::Registry>();
            exposer_->RegisterCollectable(registry_);
            
            // Define histogram buckets for latency metrics
            std::vector<double> latency_buckets = {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000};
            std::vector<double> time_buckets = {0.1, 0.5, 1, 2, 5, 10, 30, 60, 120};
            
            // Register metrics families with proper constructors
            auto& active_gauge_family = prometheus::BuildGauge()
                .Name("turtle_active_count")
                .Help("Current number of active turtles")
                .Register(*registry_);
            active_turtles_gauge_ = &active_gauge_family.Add({});
            
            auto& target_gauge_family = prometheus::BuildGauge()
                .Name("turtle_target_count")
                .Help("Target number of turtles")
                .Register(*registry_);
            target_turtles_gauge_ = &target_gauge_family.Add({});
            
            auto& spawn_counter_family = prometheus::BuildCounter()
                .Name("turtle_spawns_total")
                .Help("Total number of spawn attempts")
                .Register(*registry_);
            spawn_counter_ = &spawn_counter_family.Add({});
            
            auto& spawn_fail_counter_family = prometheus::BuildCounter()
                .Name("turtle_spawn_failures_total")
                .Help("Total number of failed spawn attempts")
                .Register(*registry_);
            spawn_fail_counter_ = &spawn_fail_counter_family.Add({});
            
            auto& kill_counter_family = prometheus::BuildCounter()
                .Name("turtle_kills_total")
                .Help("Total number of kill attempts")
                .Register(*registry_);
            kill_counter_ = &kill_counter_family.Add({});
            
            auto& kill_fail_counter_family = prometheus::BuildCounter()
                .Name("turtle_kill_failures_total")
                .Help("Total number of failed kill attempts")
                .Register(*registry_);
            kill_fail_counter_ = &kill_fail_counter_family.Add({});
            
            // Fix: Histogram requires bucket boundaries
            auto& spawn_latency_histogram_family = prometheus::BuildHistogram()
                .Name("turtle_spawn_latency_ms")
                .Help("Spawn operation latency in milliseconds")
                .Register(*registry_);
            spawn_latency_histogram_ = &spawn_latency_histogram_family.Add(
                {}, prometheus::Histogram::BucketBoundaries(latency_buckets));
            
            auto& kill_latency_histogram_family = prometheus::BuildHistogram()
                .Name("turtle_kill_latency_ms")
                .Help("Kill operation latency in milliseconds")
                .Register(*registry_);
            kill_latency_histogram_ = &kill_latency_histogram_family.Add(
                {}, prometheus::Histogram::BucketBoundaries(latency_buckets));
            
            auto& pending_gauge_family = prometheus::BuildGauge()
                .Name("turtle_pending_spawns")
                .Help("Number of pending spawn operations")
                .Register(*registry_);
            pending_spawns_gauge_ = &pending_gauge_family.Add({});
            
            auto& deviation_gauge_family = prometheus::BuildGauge()
                .Name("turtle_count_deviation")
                .Help("Deviation from target turtle count")
                .Register(*registry_);
            deviation_gauge_ = &deviation_gauge_family.Add({});
            
            auto& error_counter_family = prometheus::BuildCounter()
                .Name("turtle_errors_total")
                .Help("Total number of errors")
                .Register(*registry_);
            error_counter_ = &error_counter_family.Add({});
            
            RCLCPP_INFO(this->get_logger(), "Prometheus initialized successfully");
        } catch (const std::exception& e) {
            RCLCPP_ERROR(this->get_logger(), "Failed to initialize Prometheus: %s", e.what());
        }
    }
    
    void publishMetrics() {
        if (!registry_) return;
        
        // Update gauges
        active_turtles_gauge_->Set(spawned_turtles_.size());
        target_turtles_gauge_->Set(target_turtle_count_);
        pending_spawns_gauge_->Set(pending_spawns_);
        
        double deviation = static_cast<double>(spawned_turtles_.size()) - target_turtle_count_;
        deviation_gauge_->Set(deviation);
        
        // Log metrics to console (for Loki)
        RCLCPP_INFO_STREAM(this->get_logger(), 
            "METRICS: active=" << spawned_turtles_.size() << 
            " target=" << target_turtle_count_ <<
            " deviation=" << deviation <<
            " pending=" << pending_spawns_ <<
            " total_spawned=" << total_spawned_ <<
            " total_removed=" << total_removed_ <<
            " failed_spawns=" << failed_spawns_ <<
            " failed_kills=" << failed_kills_);
    }
    
    void removedCallback(const my_robot_interfaces::msg::TurtleInfo::SharedPtr msg) {
        auto start_time = std::chrono::steady_clock::now();
        
        if (msg->action != "removed") return;
        
        std::string turtle_name = msg->name;
        
        RCLCPP_INFO(this->get_logger(), "REMOVED CALLBACK: Turtle %s was removed!", turtle_name.c_str());
        
        auto it = spawned_turtles_.find(turtle_name);
        if (it != spawned_turtles_.end()) {
            spawned_turtles_.erase(it);
            total_removed_++;
            
            double lifetime = (std::chrono::steady_clock::now() - start_time).count() / 1e9;
            RCLCPP_INFO_STREAM(this->get_logger(), 
                "Turtle lifetime: " << turtle_name << " lived for " << lifetime << " seconds");
            
            RCLCPP_INFO(this->get_logger(), "Removed %s from internal list. Current count: %zu/%ld", 
                       turtle_name.c_str(), spawned_turtles_.size(), target_turtle_count_);
            
            // Immediate reaction to removal
            if (auto_replace_enabled_) {
                size_t current = spawned_turtles_.size();
                if (current < static_cast<size_t>(target_turtle_count_)) {
                    size_t needed = target_turtle_count_ - current;
                    RCLCPP_INFO(this->get_logger(), "Need %zu more turtles immediately!", needed);
                    pending_spawns_ = needed;
                    if (!is_spawning_) {
                        spawnNextTurtle();
                    }
                }
            }
        } else {
            RCLCPP_WARN(this->get_logger(), "Turtle %s not found in internal list!", turtle_name.c_str());
            if (error_counter_) error_counter_->Increment();
        }
    }
    
    void maintainTurtleCount() {
        if (is_spawning_) {
            RCLCPP_DEBUG(this->get_logger(), "Spawn already in progress, skipping...");
            return;
        }
        
        size_t current_turtles = spawned_turtles_.size();
        RCLCPP_INFO(this->get_logger(), "Maintain count: current=%zu, target=%ld", 
                   current_turtles, target_turtle_count_);
        
        if (current_turtles < static_cast<size_t>(target_turtle_count_)) {
            size_t needed = target_turtle_count_ - current_turtles;
            RCLCPP_INFO(this->get_logger(), "Need %zu more turtle(s) to reach target", needed);
            
            pending_spawns_ = needed;
            is_spawning_ = true;
            spawnNextTurtle();
        }
    }
    
    void spawnNextTurtle() {
        if (pending_spawns_ <= 0) {
            is_spawning_ = false;
            return;
        }
        
        spawnNewTurtle();
        pending_spawns_--;
        
        if (pending_spawns_ > 0) {
            auto next_spawn_timer = this->create_wall_timer(
                std::chrono::seconds(spawn_interval_seconds_),
                [this]() {
                    spawnNextTurtle();
                });
            pending_timers_.push_back(next_spawn_timer);
        } else {
            is_spawning_ = false;
        }
    }
    
    void spawnNewTurtle() {
        auto spawn_start = std::chrono::steady_clock::now();
        int next_id = next_turtle_id_++;
        std::string turtle_name = "turt_" + std::to_string(next_id);
        
        auto request = std::make_shared<turtlesim::srv::Spawn::Request>();
        
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> x_dist(1.0, 10.0);
        std::uniform_real_distribution<> y_dist(1.0, 10.0);
        std::uniform_real_distribution<> theta_dist(0.0, 2 * M_PI);
        
        request->x = x_dist(gen);
        request->y = y_dist(gen);
        request->theta = theta_dist(gen);
        request->name = turtle_name;
        
        RCLCPP_INFO(this->get_logger(), "Spawning turtle: %s at (%.2f, %.2f)", 
                   turtle_name.c_str(), request->x, request->y);
        
        auto result_future = spawn_client_->async_send_request(
            request,
            [this, turtle_name, x = request->x, y = request->y, theta = request->theta, spawn_start]
            (rclcpp::Client<turtlesim::srv::Spawn>::SharedFuture future) {
                spawnResponseCallback(future, turtle_name, x, y, theta, spawn_start);
            });
    }
    
    void spawnResponseCallback(rclcpp::Client<turtlesim::srv::Spawn>::SharedFuture future,
                               std::string turtle_name, float x, float y, float theta,
                               std::chrono::time_point<std::chrono::steady_clock> start_time) {
        auto end_time = std::chrono::steady_clock::now();
        double latency_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        
        try {
            auto response = future.get();
            if (!response->name.empty()) {
                spawned_turtles_.insert(turtle_name);
                total_spawned_++;
                
                // Record Prometheus metrics
                if (spawn_counter_) spawn_counter_->Increment();
                if (spawn_latency_histogram_) spawn_latency_histogram_->Observe(latency_ms);
                
                auto msg = my_robot_interfaces::msg::TurtleInfo();
                msg.name = turtle_name;
                msg.x = x;
                msg.y = y;
                msg.theta = theta;
                msg.action = "spawned";
                msg.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                
                RCLCPP_INFO_STREAM(this->get_logger(), 
                    "✓ Spawned " << turtle_name << 
                    " (Total: " << spawned_turtles_.size() << "/" << target_turtle_count_ << 
                    ") in " << latency_ms << "ms");
                
                spawned_pub_->publish(msg);
            } else {
                failed_spawns_++;
                if (spawn_fail_counter_) spawn_fail_counter_->Increment();
                if (error_counter_) error_counter_->Increment();
                
                RCLCPP_WARN_STREAM(this->get_logger(), 
                    "✗ Failed to spawn " << turtle_name << " after " << latency_ms << "ms");
                next_turtle_id_--; 
            }
        } catch (const std::exception& e) {
            failed_spawns_++;
            if (spawn_fail_counter_) spawn_fail_counter_->Increment();
            if (error_counter_) error_counter_->Increment();
            
            RCLCPP_ERROR_STREAM(this->get_logger(), 
                "Spawn failed for " << turtle_name << ": " << e.what() << " (latency=" << latency_ms << "ms)");
            next_turtle_id_--;
        }
    }
    
    void handleRemoveTurtle(const std::shared_ptr<my_robot_interfaces::srv::RemoveTurtle::Request> request,
                           std::shared_ptr<my_robot_interfaces::srv::RemoveTurtle::Response> response) {
        std::string turtle_name = request->name;
        
        RCLCPP_INFO(this->get_logger(), "Remove request for: %s", turtle_name.c_str());
        
        if (turtle_name == "turtle1") {
            response->success = false;
            response->message = "Cannot remove main turtle 'turtle1'";
            RCLCPP_WARN(this->get_logger(), "%s", response->message.c_str());
            if (error_counter_) error_counter_->Increment();
            return;
        }
        
        if (spawned_turtles_.find(turtle_name) == spawned_turtles_.end()) {
            response->success = false;
            response->message = "Turtle '" + turtle_name + "' not found";
            RCLCPP_WARN(this->get_logger(), "%s", response->message.c_str());
            if (error_counter_) error_counter_->Increment();
            return;
        }
        
        bool removed = removeTurtleInternal(turtle_name, false);
        
        if (removed) {
            response->success = true;
            response->message = "Turtle '" + turtle_name + "' removed";
            RCLCPP_INFO(this->get_logger(), "Successfully removed %s via service", turtle_name.c_str());
            
            if (auto_replace_enabled_) {
                RCLCPP_INFO(this->get_logger(), "Auto-replacing removed turtle...");
                spawnNewTurtle();
            }
        } else {
            response->success = false;
            response->message = "Failed to remove '" + turtle_name + "'";
            if (error_counter_) error_counter_->Increment();
            RCLCPP_ERROR(this->get_logger(), "%s", response->message.c_str());
        }
    }
    
    bool removeTurtleInternal(const std::string& turtle_name, bool silent) {
        auto kill_start = std::chrono::steady_clock::now();
        
        auto kill_request = std::make_shared<turtlesim::srv::Kill::Request>();
        kill_request->name = turtle_name;
        
        if (kill_counter_) kill_counter_->Increment();
        
        auto result = kill_client_->async_send_request(kill_request);
        
        if (rclcpp::spin_until_future_complete(this->get_node_base_interface(), result) == 
            rclcpp::FutureReturnCode::SUCCESS) {
            try {
                result.get();
                spawned_turtles_.erase(turtle_name);
                
                auto end_time = std::chrono::steady_clock::now();
                double latency_ms = std::chrono::duration<double, std::milli>(end_time - kill_start).count();
                
                if (kill_latency_histogram_) kill_latency_histogram_->Observe(latency_ms);
                
                if (!silent) {
                    auto msg = my_robot_interfaces::msg::TurtleInfo();
                    msg.name = turtle_name;
                    msg.x = 0.0;
                    msg.y = 0.0;
                    msg.theta = 0.0;
                    msg.action = "removed";
                    msg.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    removed_pub_->publish(msg);
                    
                    RCLCPP_INFO_STREAM(this->get_logger(), 
                        "✓ Removed " << turtle_name << 
                        " (Total: " << spawned_turtles_.size() << "/" << target_turtle_count_ << 
                        ") in " << latency_ms << "ms");
                }
                return true;
            } catch (const std::exception& e) {
                failed_kills_++;
                if (kill_fail_counter_) kill_fail_counter_->Increment();
                if (error_counter_) error_counter_->Increment();
                
                if (!silent) {
                    RCLCPP_ERROR_STREAM(this->get_logger(), 
                        "Failed to remove " << turtle_name << ": " << e.what());
                }
                return false;
            }
        }
        
        failed_kills_++;
        if (kill_fail_counter_) kill_fail_counter_->Increment();
        if (error_counter_) error_counter_->Increment();
        return false;
    }
    
    // ROS2 Communication
    rclcpp::Client<turtlesim::srv::Spawn>::SharedPtr spawn_client_;
    rclcpp::Client<turtlesim::srv::Kill>::SharedPtr kill_client_;
    rclcpp::Service<my_robot_interfaces::srv::RemoveTurtle>::SharedPtr remove_service_;
    rclcpp::Publisher<my_robot_interfaces::msg::TurtleInfo>::SharedPtr spawned_pub_;
    rclcpp::Publisher<my_robot_interfaces::msg::TurtleInfo>::SharedPtr removed_pub_;
    rclcpp::Subscription<my_robot_interfaces::msg::TurtleInfo>::SharedPtr removed_sub_;
    rclcpp::TimerBase::SharedPtr spawn_timer_;
    rclcpp::TimerBase::SharedPtr metrics_timer_;
    std::vector<rclcpp::TimerBase::SharedPtr> pending_timers_;
    
    // State
    std::set<std::string> spawned_turtles_;
    int next_turtle_id_ = 1;
    long target_turtle_count_;
    int spawn_interval_seconds_;
    bool auto_replace_enabled_;
    int pending_spawns_;
    bool is_spawning_;
    
    // Statistics
    int total_spawned_;
    int total_removed_;
    int failed_spawns_;
    int failed_kills_;
    
    // Prometheus
    std::unique_ptr<prometheus::Exposer> exposer_;
    std::shared_ptr<prometheus::Registry> registry_;
    prometheus::Gauge* active_turtles_gauge_;
    prometheus::Gauge* target_turtles_gauge_;
    prometheus::Gauge* pending_spawns_gauge_;
    prometheus::Gauge* deviation_gauge_;
    prometheus::Counter* spawn_counter_;
    prometheus::Counter* spawn_fail_counter_;
    prometheus::Counter* kill_counter_;
    prometheus::Counter* kill_fail_counter_;
    prometheus::Counter* error_counter_;
    prometheus::Histogram* spawn_latency_histogram_;
    prometheus::Histogram* kill_latency_histogram_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<TurtleManager>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}