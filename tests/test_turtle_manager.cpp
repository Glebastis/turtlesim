#include <gtest/gtest.h>
#include <memory>
#include "rclcpp/rclcpp.hpp"
#include "my_robot_interfaces/msg/turtle_info.hpp"

class TurtleManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        rclcpp::init(0, nullptr);
        node_ = std::make_shared<rclcpp::Node>("test_turtle_manager");
    }
    
    void TearDown() override {
        node_.reset();
        rclcpp::shutdown();
    }
    
    rclcpp::Node::SharedPtr node_;
};

TEST_F(TurtleManagerTest, TurtleInfoMessageCreation) {
    auto msg = my_robot_interfaces::msg::TurtleInfo();
    msg.name = "test_turtle";
    msg.x = 5.0;
    msg.y = 5.0;
    msg.theta = 1.57;
    msg.action = "spawned";
    
    EXPECT_EQ(msg.name, "test_turtle");
    EXPECT_FLOAT_EQ(msg.x, 5.0);
    EXPECT_FLOAT_EQ(msg.y, 5.0);
    EXPECT_TRUE(msg.timestamp > 0);
}

TEST_F(TurtleManagerTest, DistanceCalculation) {
    // Тест расчета дистанции между черепашками
    double x1 = 0, y1 = 0;
    double x2 = 3, y2 = 4;
    double distance = std::hypot(x2 - x1, y2 - y1);
    
    EXPECT_FLOAT_EQ(distance, 5.0);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}