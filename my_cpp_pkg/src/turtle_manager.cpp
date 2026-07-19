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

class TurtleManager : public rclcpp::Node {
public:
    TurtleManager() : Node("turtle_manager"), 
                      target_turtle_count_(5),
                      spawn_interval_seconds_(1),  // Уменьшил до 1 секунды
                      auto_replace_enabled_(true),
                      pending_spawns_(0),
                      is_spawning_(false) {
                          
        this->declare_parameter<int>("target_turtle_count", 5);
        this->declare_parameter<int>("spawn_interval_seconds", 1);  // Изменено
        this->declare_parameter<bool>("auto_replace_enabled", true);
        
        target_turtle_count_ = this->get_parameter("target_turtle_count").as_int();
        spawn_interval_seconds_ = this->get_parameter("spawn_interval_seconds").as_int();
        auto_replace_enabled_ = this->get_parameter("auto_replace_enabled").as_bool();
        
        // Клиенты для turtlesim
        spawn_client_ = this->create_client<turtlesim::srv::Spawn>("/spawn");
        kill_client_ = this->create_client<turtlesim::srv::Kill>("/kill");
        
        // Сервис для удаления черепашки
        remove_service_ = this->create_service<my_robot_interfaces::srv::RemoveTurtle>(
            "/remove_turtle",
            std::bind(&TurtleManager::handleRemoveTurtle, this,
                     std::placeholders::_1, std::placeholders::_2));
        
        // Публикаторы для событий
        spawned_pub_ = this->create_publisher<my_robot_interfaces::msg::TurtleInfo>("/spawned", 10);
        removed_pub_ = this->create_publisher<my_robot_interfaces::msg::TurtleInfo>("/removed", 10);
        
        // ПОДПИСКА НА УДАЛЕНИЯ
        removed_sub_ = this->create_subscription<my_robot_interfaces::msg::TurtleInfo>(
            "/removed", 10,
            std::bind(&TurtleManager::removedCallback, this, std::placeholders::_1));
        
        // Запускаем процесс спавна - сразу активный
        spawn_timer_ = this->create_wall_timer(
            std::chrono::seconds(spawn_interval_seconds_),
            std::bind(&TurtleManager::maintainTurtleCount, this));
        
        // Даем время turtlesim_node на запуск
        while (!spawn_client_->wait_for_service(std::chrono::seconds(1))) {
            RCLCPP_WARN(this->get_logger(), "Waiting for turtlesim spawn service...");
            if (!rclcpp::ok()) {
                RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for service");
                return;
            }
        }
        
        // Запускаем начальный спавн
        maintainTurtleCount();
        
        RCLCPP_INFO(this->get_logger(), "TurtleManager initialized. Target: %ld turtles", 
                   target_turtle_count_);
        RCLCPP_INFO(this->get_logger(), "Spawn interval: %d seconds", spawn_interval_seconds_);
        RCLCPP_INFO(this->get_logger(), "Auto-replace: %s", 
                   auto_replace_enabled_ ? "ENABLED" : "DISABLED");
    }

private:
    void removedCallback(const my_robot_interfaces::msg::TurtleInfo::SharedPtr msg) {
        if (msg->action != "removed") return;
        
        std::string turtle_name = msg->name;
        
        RCLCPP_INFO(this->get_logger(), "REMOVED CALLBACK: Turtle %s was removed!", turtle_name.c_str());
        
        auto it = spawned_turtles_.find(turtle_name);
        if (it != spawned_turtles_.end()) {
            spawned_turtles_.erase(it);
            RCLCPP_INFO(this->get_logger(), "Removed %s from internal list. Current count: %zu/%ld", 
                       turtle_name.c_str(), spawned_turtles_.size(), target_turtle_count_);
            
            // НЕМЕДЛЕННО реагируем на удаление
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
        }
    }
    
    void maintainTurtleCount() {
        // Если уже идет процесс спавна, не запускаем новый
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
        
        // Если нужно спавнить еще черепашек, запускаем следующий
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
            [this, turtle_name, x = request->x, y = request->y, theta = request->theta]
            (rclcpp::Client<turtlesim::srv::Spawn>::SharedFuture future) {
                spawnResponseCallback(future, turtle_name, x, y, theta);
            });
    }
    
    void spawnResponseCallback(rclcpp::Client<turtlesim::srv::Spawn>::SharedFuture future,
                               std::string turtle_name, float x, float y, float theta) {
        try {
            auto response = future.get();
            if (!response->name.empty()) {
                spawned_turtles_.insert(turtle_name);
                
                auto msg = my_robot_interfaces::msg::TurtleInfo();
                msg.name = turtle_name;
                msg.x = x;
                msg.y = y;
                msg.theta = theta;
                msg.action = "spawned";
                msg.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                
                RCLCPP_INFO(this->get_logger(), "Publishing to /spawned: %s at (%.2f, %.2f)", 
                           turtle_name.c_str(), x, y);
                
                spawned_pub_->publish(msg);
                
                RCLCPP_INFO(this->get_logger(), "✓ Spawned %s (Total: %zu/%ld)", 
                           turtle_name.c_str(), spawned_turtles_.size(), target_turtle_count_);
            } else {
                RCLCPP_WARN(this->get_logger(), "✗ Failed to spawn %s", turtle_name.c_str());
                next_turtle_id_--; 
            }
        } catch (const std::exception& e) {
            RCLCPP_ERROR(this->get_logger(), "Spawn failed: %s", e.what());
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
            return;
        }
        
        if (spawned_turtles_.find(turtle_name) == spawned_turtles_.end()) {
            response->success = false;
            response->message = "Turtle '" + turtle_name + "' not found";
            RCLCPP_WARN(this->get_logger(), "%s", response->message.c_str());
            return;
        }
        
        bool removed = removeTurtleInternal(turtle_name, false);
        
        if (removed) {
            response->success = true;
            response->message = "Turtle '" + turtle_name + "' removed";
            
            if (auto_replace_enabled_) {
                RCLCPP_INFO(this->get_logger(), "Auto-replacing removed turtle...");
                spawnNewTurtle();
            }
        } else {
            response->success = false;
            response->message = "Failed to remove '" + turtle_name + "'";
        }
    }
    
    bool removeTurtleInternal(const std::string& turtle_name, bool silent) {
        auto kill_request = std::make_shared<turtlesim::srv::Kill::Request>();
        kill_request->name = turtle_name;
        
        auto result = kill_client_->async_send_request(kill_request);
        
        if (rclcpp::spin_until_future_complete(this->get_node_base_interface(), result) == 
            rclcpp::FutureReturnCode::SUCCESS) {
            try {
                result.get();
                spawned_turtles_.erase(turtle_name);
                
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
                    
                    RCLCPP_INFO(this->get_logger(), "✓ Removed %s (Total: %zu/%ld)", 
                               turtle_name.c_str(), spawned_turtles_.size(), target_turtle_count_);
                }
                return true;
            } catch (const std::exception& e) {
                if (!silent) {
                    RCLCPP_ERROR(this->get_logger(), "Failed to remove %s: %s", 
                                turtle_name.c_str(), e.what());
                }
                return false;
            }
        }
        
        return false;
    }
    
    rclcpp::Client<turtlesim::srv::Spawn>::SharedPtr spawn_client_;
    rclcpp::Client<turtlesim::srv::Kill>::SharedPtr kill_client_;
    rclcpp::Service<my_robot_interfaces::srv::RemoveTurtle>::SharedPtr remove_service_;
    rclcpp::Publisher<my_robot_interfaces::msg::TurtleInfo>::SharedPtr spawned_pub_;
    rclcpp::Publisher<my_robot_interfaces::msg::TurtleInfo>::SharedPtr removed_pub_;
    rclcpp::Subscription<my_robot_interfaces::msg::TurtleInfo>::SharedPtr removed_sub_;
    rclcpp::TimerBase::SharedPtr spawn_timer_;
    std::vector<rclcpp::TimerBase::SharedPtr> pending_timers_;
    
    std::set<std::string> spawned_turtles_;
    int next_turtle_id_ = 1;
    long target_turtle_count_;
    int spawn_interval_seconds_;
    bool auto_replace_enabled_;
    int pending_spawns_;
    bool is_spawning_;  // Флаг, чтобы избежать множественных спавнов
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<TurtleManager>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}