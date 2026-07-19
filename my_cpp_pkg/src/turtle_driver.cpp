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

class TurtleController : public rclcpp::Node {
public:
    TurtleController() : Node("turtle_controller"),
                         has_target_(false),
                         eat_radius_(0.5),
                         arrival_distance_(0.3),
                         linear_speed_(1.0),
                         max_angular_speed_(1.5),
                         angular_gain_(1.5) {
        
        // Подписка на позицию главной черепашки
        pose_sub_ = this->create_subscription<turtlesim::msg::Pose>(
            "/turtle1/pose", 10,
            std::bind(&TurtleController::poseCallback, this, std::placeholders::_1));
        
        // Подписка на спавн новых черепашек
        spawn_sub_ = this->create_subscription<my_robot_interfaces::msg::TurtleInfo>(
            "/spawned", 10,
            std::bind(&TurtleController::spawnCallback, this, std::placeholders::_1));
        
        // Подписка на удаление черепашек (для синхронизации)
        removed_sub_ = this->create_subscription<my_robot_interfaces::msg::TurtleInfo>(
            "/removed", 10,
            std::bind(&TurtleController::removedCallback, this, std::placeholders::_1));
        
        // Публикатор команд скорости
        cmd_vel_pub_ = this->create_publisher<geometry_msgs::msg::Twist>("/turtle1/cmd_vel", 10);
        
        // Публикатор для удалений (ВАЖНО!)
        removed_pub_ = this->create_publisher<my_robot_interfaces::msg::TurtleInfo>("/removed", 10);
        
        // Клиент для убийства черепашек
        kill_client_ = this->create_client<turtlesim::srv::Kill>("/kill");
        
        // Таймер управления (10 Гц)
        control_timer_ = this->create_wall_timer(
            std::chrono::milliseconds(100),
            std::bind(&TurtleController::controlLoop, this));
        
        RCLCPP_INFO(this->get_logger(), "TurtleController started. Waiting for turtles...");
    }

private:
    // Структура для хранения информации о черепашке
    struct TurtleData {
        std::string name;
        double x;
        double y;
        double distance;  // расстояние от главной черепашки
    };
    
    // Колбэк позиции
    void poseCallback(const turtlesim::msg::Pose::SharedPtr msg) {
        current_pose_ = *msg;
        updateAllDistances();  // обновляем расстояния до всех черепашек
        
        // Если есть цель, проверяем, не стала ли какая-то другая черепашка ближе
        if (has_target_ && !turtles_.empty()) {
            auto it = turtles_.find(current_target_);
            if (it != turtles_.end()) {
                // Проверяем, нет ли черепашки ближе текущей цели
                for (const auto& pair : turtles_) {
                    if (pair.first != current_target_ && pair.second.distance < it->second.distance - 0.1) {
                        // Нашли более близкую черепашку - переключаем цель
                        RCLCPP_INFO(this->get_logger(), "Found closer turtle: %s (dist=%.2f) vs current target %s (dist=%.2f)",
                                    pair.first.c_str(), pair.second.distance,
                                    current_target_.c_str(), it->second.distance);
                        current_target_ = pair.first;
                        break;
                    }
                }
            } else {
                // Текущей цели больше нет в списке
                has_target_ = false;
                selectNearestTarget();
            }
        } else if (!has_target_ && !turtles_.empty()) {
            // Нет цели, но есть черепашки - выбираем ближайшую
            selectNearestTarget();
        }
    }
    
    // Колбэк спавна
    void spawnCallback(const my_robot_interfaces::msg::TurtleInfo::SharedPtr msg) {
        if (msg->action != "spawned") return;
        
        // Игнорируем, если черепашка уже есть (на всякий случай)
        if (turtles_.find(msg->name) != turtles_.end()) return;
        
        TurtleData data;
        data.name = msg->name;
        data.x = msg->x;
        data.y = msg->y;
        data.distance = distanceTo(data);
        
        turtles_[msg->name] = data;
        
        RCLCPP_INFO(this->get_logger(), "New turtle spawned: %s at (%.2f, %.2f), dist=%.2f. Total turtles: %zu",
                    data.name.c_str(), data.x, data.y, data.distance, turtles_.size());
        
        // Если у нас нет цели, выбираем ближайшую
        if (!has_target_) {
            selectNearestTarget();
        } else {
            // Проверяем, не стала ли новая черепашка ближе текущей цели
            auto current_it = turtles_.find(current_target_);
            if (current_it != turtles_.end() && data.distance < current_it->second.distance - 0.1) {
                RCLCPP_INFO(this->get_logger(), "New turtle %s is closer than current target %s, switching!",
                            data.name.c_str(), current_target_.c_str());
                current_target_ = data.name;
            }
        }
    }
    
    // Колбэк удаления (синхронизация)
    void removedCallback(const my_robot_interfaces::msg::TurtleInfo::SharedPtr msg) {
        if (msg->action != "removed") return;
        auto it = turtles_.find(msg->name);
        if (it != turtles_.end()) {
            RCLCPP_INFO(this->get_logger(), "External removal detected: %s", msg->name.c_str());
            turtles_.erase(it);
            if (current_target_ == msg->name) {
                current_target_.clear();
                has_target_ = false;
                selectNearestTarget();
            }
        }
    }
    
    // Пересчёт расстояний от главной черепашки до всех сохранённых
    void updateAllDistances() {
        for (auto& pair : turtles_) {
            pair.second.distance = distanceTo(pair.second);
        }
    }
    
    // Расстояние от главной черепашки до указанной черепашки
    double distanceTo(const TurtleData& other) {
        double dx = other.x - current_pose_.x;
        double dy = other.y - current_pose_.y;
        return std::hypot(dx, dy);
    }
    
    // Выбор ближайшей черепашки в качестве цели
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
    
    // Убить черепашку по имени (асинхронная версия)
    void killTurtle(const std::string& name) {
        RCLCPP_INFO(this->get_logger(), "Attempting to kill turtle: %s", name.c_str());
        
        auto request = std::make_shared<turtlesim::srv::Kill::Request>();
        request->name = name;
        
        // Асинхронный вызов с колбэком
        auto future = kill_client_->async_send_request(
            request,
            [this, name](rclcpp::Client<turtlesim::srv::Kill>::SharedFuture future) {
                try {
                    future.get();
                    RCLCPP_INFO(this->get_logger(), "Successfully killed turtle: %s", name.c_str());
                    
                    // Публикуем сообщение об удалении
                    auto remove_msg = my_robot_interfaces::msg::TurtleInfo();
                    remove_msg.name = name;
                    remove_msg.x = 0.0;
                    remove_msg.y = 0.0;
                    remove_msg.theta = 0.0;
                    remove_msg.action = "removed";
                    remove_msg.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    removed_pub_->publish(remove_msg);
                    RCLCPP_INFO(this->get_logger(), "Published removal to /removed: %s", name.c_str());
                    
                    // Удаляем из нашей таблицы
                    turtles_.erase(name);
                    RCLCPP_INFO(this->get_logger(), "Removed from list. Remaining turtles: %zu", turtles_.size());
                    
                    if (current_target_ == name) {
                        current_target_.clear();
                        has_target_ = false;
                        // Выбираем следующую цель, если есть другие черепашки
                        if (!turtles_.empty()) {
                            RCLCPP_INFO(this->get_logger(), "Selecting next target from %zu remaining turtles", turtles_.size());
                            selectNearestTarget();
                        } else {
                            RCLCPP_INFO(this->get_logger(), "No turtles remaining, will circle");
                        }
                    }
                } catch (const std::exception& e) {
                    RCLCPP_ERROR(this->get_logger(), "Failed to kill turtle %s: %s", name.c_str(), e.what());
                }
            });
    }
    
    // Проверка, достигла ли главная черепашка цели (в пределах окружности)
    void checkAndEat() {
        if (!has_target_) return;
        auto it = turtles_.find(current_target_);
        if (it == turtles_.end()) {
            // Цели больше нет
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
    
    // Управление движением к цели или по кругу
    void controlLoop() {
        checkAndEat();  // сначала проверяем, не съели ли цель
        
        if (!has_target_ || turtles_.empty()) {
            // Движение по кругу
            geometry_msgs::msg::Twist cmd;
            cmd.linear.x = linear_speed_;
            cmd.angular.z = 0.8;   // постоянная угловая скорость -> окружность
            cmd_vel_pub_->publish(cmd);
            return;
        }
        
        // Движение к текущей цели
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
        
        // Разница углов
        double angle_diff = target_angle - current_pose_.theta;
        // Нормализация в [-π, π]
        while (angle_diff > M_PI) angle_diff -= 2 * M_PI;
        while (angle_diff < -M_PI) angle_diff += 2 * M_PI;
        
        geometry_msgs::msg::Twist cmd;
        
        // Если почти смотрим на цель, едем вперёд; иначе поворачиваемся на месте
        if (std::abs(angle_diff) < 0.2) {
            // Движемся вперёд с линейной скоростью
            cmd.linear.x = linear_speed_;
            // Подруливаем, чтобы компенсировать небольшие отклонения
            cmd.angular.z = angular_gain_ * angle_diff;
            // Ограничиваем угловую скорость
            cmd.angular.z = std::clamp(cmd.angular.z, -max_angular_speed_, max_angular_speed_);
        } else {
            // Поворачиваем на месте
            cmd.linear.x = 0.0;
            cmd.angular.z = angular_gain_ * angle_diff;
            cmd.angular.z = std::clamp(cmd.angular.z, -max_angular_speed_, max_angular_speed_);
        }
        
        cmd_vel_pub_->publish(cmd);
    }
    
    // ROS2 коммуникации
    rclcpp::Subscription<turtlesim::msg::Pose>::SharedPtr pose_sub_;
    rclcpp::Subscription<my_robot_interfaces::msg::TurtleInfo>::SharedPtr spawn_sub_;
    rclcpp::Subscription<my_robot_interfaces::msg::TurtleInfo>::SharedPtr removed_sub_;
    rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_pub_;
    rclcpp::Publisher<my_robot_interfaces::msg::TurtleInfo>::SharedPtr removed_pub_;  // ДОБАВЛЕНО
    rclcpp::Client<turtlesim::srv::Kill>::SharedPtr kill_client_;
    rclcpp::TimerBase::SharedPtr control_timer_;
    
    // Состояние
    turtlesim::msg::Pose current_pose_;
    std::map<std::string, TurtleData> turtles_;
    std::string current_target_;
    bool has_target_;
    
    // Параметры
    const double eat_radius_;          // радиус поедания
    const double arrival_distance_;    // дистанция активации поедания (используется eat_radius_)
    const double linear_speed_;        // максимальная линейная скорость
    const double max_angular_speed_;   // максимальная угловая скорость
    const double angular_gain_;        // коэффициент поворота
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<TurtleController>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}