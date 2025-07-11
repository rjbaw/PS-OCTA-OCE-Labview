/**
 * @file focus_node.cpp
 * @author rjbaw
 * @brief Node that focuses robot end effector to the normal of the target
 */

#include <cmath>
#include <condition_variable>
#include <format>
#include <mutex>
#include <open3d/Open3D.h>
#include <opencv2/img_hash.hpp>
#include <opencv2/opencv.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

#include <geometry_msgs/msg/pose_stamped.hpp>
#include <tf2_eigen/tf2_eigen.hpp>
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>

#include <moveit/moveit_cpp/moveit_cpp.hpp>
#include <moveit/moveit_cpp/planning_component.hpp>
#include <moveit_msgs/msg/move_it_error_codes.hpp>

#include <octa_ros/action/focus.hpp>
#include <octa_ros/msg/img.hpp>
#include <octa_ros/srv/scan3d.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <ament_index_cpp/get_package_share_directory.hpp>

#include "process_img.hpp"
#include "utils.hpp"

using namespace std::chrono_literals;

class FocusActionServer : public rclcpp::Node {
    using Focus = octa_ros::action::Focus;
    using GoalHandleFocus = rclcpp_action::ServerGoalHandle<Focus>;
    using Scan3d = octa_ros::srv::Scan3d;

  public:
    FocusActionServer(
        const rclcpp::NodeOptions &options = rclcpp::NodeOptions())
        : Node("focus_action_server",
               rclcpp::NodeOptions(options)
                   .automatically_declare_parameters_from_overrides(true)) {}

    void init() {
        action_server_ = rclcpp_action::create_server<Focus>(
            this, "focus_action",
            std::bind(&FocusActionServer::handle_goal, this,
                      std::placeholders::_1, std::placeholders::_2),
            std::bind(&FocusActionServer::handle_cancel, this,
                      std::placeholders::_1),
            std::bind(&FocusActionServer::handle_accepted, this,
                      std::placeholders::_1));

        moveit_cpp_ =
            std::make_shared<moveit_cpp::MoveItCpp>(shared_from_this());
        tem_ = moveit_cpp_->getTrajectoryExecutionManagerNonConst();
        planning_component_ = std::make_shared<moveit_cpp::PlanningComponent>(
            "ur_manipulator", moveit_cpp_);

        last_store_time_ =
            now() - rclcpp::Duration::from_seconds(gating_interval_);
        img_subscriber_ = create_subscription<octa_ros::msg::Img>(
            "oct_image", rclcpp::QoS(rclcpp::KeepLast(10)).best_effort(),
            std::bind(&FocusActionServer::imageCallback, this,
                      std::placeholders::_1));
        img_timer_ = this->create_wall_timer(
            std::chrono::milliseconds(10),
            std::bind(&FocusActionServer::imageTimerCallback, this));
        img_timer_->cancel();

        service_scan_3d_ = create_client<Scan3d>("scan_3d");

        capture_background_srv_ = create_service<std_srvs::srv::Trigger>(
            "capture_background",
            std::bind(&FocusActionServer::captureBackgroundCallback, this,
                      std::placeholders::_1, std::placeholders::_2));
    }

  private:
    bool early_terminate_ = false;
    bool skip_angle_tolerance_ = true;

    rclcpp_action::Server<Focus>::SharedPtr action_server_;
    std::shared_ptr<GoalHandleFocus> active_goal_handle_;

    moveit_cpp::MoveItCppPtr moveit_cpp_;
    std::shared_ptr<moveit_cpp::PlanningComponent> planning_component_;
    std::shared_ptr<trajectory_execution_manager::TrajectoryExecutionManager>
        tem_;

    cv::Mat img_;
    cv::Mat img_hash_;
    rclcpp::Time last_store_time_;
    std::mutex img_mutex_;
    std::condition_variable img_cv_;
    rclcpp::Subscription<octa_ros::msg::Img>::SharedPtr img_subscriber_;
    rclcpp::TimerBase::SharedPtr img_timer_;
    uint64_t img_seq_ = 0;
    uint64_t last_read_seq_ = 0;

    std::vector<cv::Mat> img_array_;
    std::vector<Eigen::Vector3d> pc_lines_;
    Eigen::Matrix3d rotmat_eigen_;
    tf2::Quaternion q_;
    tf2::Quaternion target_q_;
    double dz_ = 0.0;

    std::string msg_;
    geometry_msgs::msg::PoseStamped target_pose_;

    bool angle_focused_ = false;
    bool z_focused_ = false;
    bool planning_ = false;

    rclcpp::Client<Scan3d>::SharedPtr service_scan_3d_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr capture_background_srv_;

    double roll_ = 0.0;
    double pitch_ = 0.0;
    double yaw_ = 0.0;
    double tmp_roll_ = 0.0;
    double tmp_pitch_ = 0.0;
    double tmp_yaw_ = 0.0;

    const double gating_interval_ = 0.05;
    const int width_ = 500;
    const int height_ = 512;
    const int interval_ = 6;
    const bool single_interval_ = false;
    const double px_per_mm = 55.0;

    double angle_tolerance_ = 0.0;
    double z_tolerance_ = 0.0;
    double z_height_ = 0.0;

    rclcpp::Time start;

    rclcpp_action::GoalResponse
    handle_goal([[maybe_unused]] const rclcpp_action::GoalUUID &uuid,
                std::shared_ptr<const Focus::Goal> goal) {
        angle_tolerance_ = goal->angle_tolerance;
        z_tolerance_ = goal->z_tolerance;
        z_height_ = goal->z_height;
        RCLCPP_INFO(get_logger(),
                    "Focus goal: angle_tolerance=%.2f deg, "
                    "z_height_tolerance=%.2f mm",
                    angle_tolerance_, z_tolerance_);
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
    }

    rclcpp_action::CancelResponse
    handle_cancel(const std::shared_ptr<GoalHandleFocus> goal_handle) {
        if (!goal_handle->is_active()) {
            RCLCPP_INFO(get_logger(), "Focus goal no longer active");
            return rclcpp_action::CancelResponse::REJECT;
        }
        auto result = std::make_shared<Focus::Result>();
        result->status = "Focus action canceled by user request\n";
        call_scan3d(false);
        tem_->stopExecution(true);
        planning_component_->setStartStateToCurrentState();
        img_timer_->cancel();
        RCLCPP_INFO(get_logger(), "Focus action canceled");
        return rclcpp_action::CancelResponse::ACCEPT;
    }

    void handle_accepted(const std::shared_ptr<GoalHandleFocus> goal_handle) {
        auto result = std::make_shared<Focus::Result>();
        result->status = "Pre-empted by new goal\n";
        RCLCPP_INFO(get_logger(), "Preempting old goal...");

        if (active_goal_handle_ && active_goal_handle_->is_active()) {
            active_goal_handle_->abort(result);
        }
        img_timer_->reset();
        active_goal_handle_ = goal_handle;
        std::thread([this, goal_handle]() { execute(goal_handle); }).detach();
    }

    moveit_msgs::msg::Constraints makeEnvelope(const Eigen::Isometry3d &centre,
                                               double lin_radius_m,
                                               double ang_radius_rad) const {
        moveit_msgs::msg::Constraints c;

        const std::string planning_frame =
            moveit_cpp_->getPlanningSceneMonitor()
                ->getPlanningScene()
                ->getPlanningFrame();

        moveit_msgs::msg::PositionConstraint pc;
        pc.header.frame_id = planning_frame;
        pc.link_name = "tcp";
        pc.weight = 1.0;
        shape_msgs::msg::SolidPrimitive sphere;
        sphere.type = sphere.SPHERE;
        sphere.dimensions = {lin_radius_m};
        pc.constraint_region.primitives.push_back(sphere);
        geometry_msgs::msg::Pose centre_pose;
        centre_pose.position.x = centre.translation().x();
        centre_pose.position.y = centre.translation().y();
        centre_pose.position.z = centre.translation().z();
        centre_pose.orientation.w = 1.0;
        pc.constraint_region.primitive_poses.push_back(centre_pose);

        moveit_msgs::msg::OrientationConstraint oc;
        oc.header.frame_id = planning_frame;
        oc.link_name = "tcp";
        oc.weight = 1.0;
        Eigen::Quaterniond q(centre.rotation());
        oc.orientation.x = q.x();
        oc.orientation.y = q.y();
        oc.orientation.z = q.z();
        oc.orientation.w = q.w();
        oc.absolute_x_axis_tolerance = oc.absolute_y_axis_tolerance =
            oc.absolute_z_axis_tolerance = ang_radius_rad;

        c.position_constraints.push_back(pc);
        c.orientation_constraints.push_back(oc);
        return c;
    }

    cv::Mat get_img() {
        std::unique_lock<std::mutex> lock(img_mutex_);
        img_cv_.wait(lock, [this]() { return (img_seq_ > last_read_seq_); });
        last_read_seq_ = img_seq_;
        return img_.clone();
    }

    void imageCallback(const octa_ros::msg::Img::SharedPtr msg) {
        auto now = this->now();
        double elapsed = (now - last_store_time_).seconds();
        if (elapsed < gating_interval_) {

            RCLCPP_DEBUG(get_logger(),
                         "Skipping frame (%.2f sec since last store)", elapsed);
            return;
        }
        RCLCPP_DEBUG(get_logger(),
                     "Storing new frame after %.2f sec (size=%zu)", elapsed,
                     msg->img.size());
        cv::Mat new_img(height_, width_, CV_8UC1);
        std::copy(msg->img.begin(), msg->img.end(), new_img.data);
        {
            std::lock_guard<std::mutex> lock(img_mutex_);
            img_ = new_img;
            ++img_seq_;
            last_store_time_ = now;
            img_cv_.notify_all();
        }
    }

    void imageTimerCallback() {
        cv::Mat image_copy;
        {
            std::lock_guard<std::mutex> lock(img_mutex_);
            if (img_.empty()) {
                RCLCPP_DEBUG(this->get_logger(),
                             "timerCallback: No image to process");
                return;
            }
            image_copy = img_.clone();
        }
        cv::Mat current_hash;
        cv::img_hash::AverageHash::create()->compute(image_copy, current_hash);
        if (img_hash_.empty()) {
            img_hash_ = current_hash.clone();
            RCLCPP_DEBUG(this->get_logger(),
                         "First hash stored. current_hash size=[%dx%d]",
                         current_hash.rows, current_hash.cols);
            return;
        }
        double diff = 0.0;
        if (img_hash_.size == current_hash.size &&
            img_hash_.type() == current_hash.type()) {
            diff = cv::norm(img_hash_, current_hash);
            RCLCPP_DEBUG(this->get_logger(),
                         "Timer triggered - processing. Hash diff=%.2f", diff);
        } else {
            RCLCPP_WARN(
                this->get_logger(),
                "Hash mismatch in size or type! old=(%d x %d, type=%d), "
                "new=(%d x %d, type=%d)",
                img_hash_.rows, img_hash_.cols, img_hash_.type(),
                current_hash.rows, current_hash.cols, current_hash.type());
        }
        RCLCPP_DEBUG(this->get_logger(),
                     "Timer triggered - final update. Hash diff=%.2f", diff);
        img_hash_ = current_hash.clone();
    }

    bool call_scan3d(bool activate) {
        if (!service_scan_3d_->wait_for_service(0s))
            return false;
        auto req = std::make_shared<Scan3d::Request>();
        req->activate = activate;
        auto fut = service_scan_3d_->async_send_request(req);
        return fut.wait_for(2s) == std::future_status::ready &&
               fut.get()->success;
    }

    bool tol_measure(const double &roll, const double &pitch,
                     const double &angle_tolerance) {
        return ((std::abs(roll) < to_radian(angle_tolerance)) &&
                (std::abs(pitch) < to_radian(angle_tolerance)));
    }

    void execute(const std::shared_ptr<GoalHandleFocus> goal_handle) {
        if (!goal_handle->is_active()) {
            return;
        }
        auto feedback = std::make_shared<Focus::Feedback>();
        auto result = std::make_shared<Focus::Result>();

        goal_handle->publish_feedback(feedback);
        angle_focused_ = false;
        z_focused_ = false;

        while (!angle_focused_ || !z_focused_) {
            if (!goal_handle->is_active()) {
                tem_->stopExecution(true);
                planning_component_->setStartStateToCurrentState();
                return;
            }
            if (goal_handle->is_canceling()) {
                tem_->stopExecution(true);
                result->status = "Cancel requested!\n";
                goal_handle->canceled(result);
                RCLCPP_INFO(get_logger(), "Cancel requested!");
                planning_component_->setStartStateToCurrentState();
                return;
            }

            img_timer_->reset();
            start = now();
            while (!call_scan3d(true)) {
                if (!goal_handle->is_active()) {
                    tem_->stopExecution(true);
                    planning_component_->setStartStateToCurrentState();
                    return;
                }
                if (goal_handle->is_canceling()) {
                    tem_->stopExecution(true);
                    result->status = "Cancel requested!\n";
                    goal_handle->canceled(result);
                    RCLCPP_INFO(get_logger(), "Cancel requested!");
                    planning_component_->setStartStateToCurrentState();
                    return;
                }
                if ((this->now() - start).seconds() > 5.0) {
                    RCLCPP_WARN(get_logger(),
                                "activate_3d_scan not responding...");
                    result->status = "activate_3d_scan timed out\n";
                    goal_handle->abort(result);
                    return;
                }
                rclcpp::sleep_for(50ms);
            }
            img_array_.clear();
            for (int i = 0; i < interval_; i++) {
                start = now();
                while (true) {
                    cv::Mat frame = get_img();
                    if (!img_.empty()) {
                        img_array_.push_back(frame);
                        break;
                    }
                    if (!goal_handle->is_active()) {
                        tem_->stopExecution(true);
                        planning_component_->setStartStateToCurrentState();
                        return;
                    }
                    if (goal_handle->is_canceling()) {
                        tem_->stopExecution(true);
                        result->status = "Cancel requested!\n";
                        goal_handle->canceled(result);
                        RCLCPP_INFO(get_logger(), "Cancel requested!");
                        planning_component_->setStartStateToCurrentState();
                        return;
                    }
                    if ((now() - start).seconds() > 5.0) {
                        RCLCPP_WARN(get_logger(),
                                    "timed out. cannot acquire image");
                        result->status = "timed out. cannot acquire image.\n";
                        goal_handle->abort(result);
                        return;
                    }
                }
                msg_ = std::format("Collected image {}", i + 1);
                RCLCPP_INFO(get_logger(), msg_.c_str());
            }

            img_timer_->cancel();
            start = now();
            while (!call_scan3d(false)) {
                if (!goal_handle->is_active()) {
                    tem_->stopExecution(true);
                    planning_component_->setStartStateToCurrentState();
                    return;
                }
                if (goal_handle->is_canceling()) {
                    tem_->stopExecution(true);
                    result->status = "Cancel requested!\n";
                    goal_handle->canceled(result);
                    RCLCPP_INFO(get_logger(), "Cancel requested!");
                    planning_component_->setStartStateToCurrentState();
                    return;
                }
                if ((now() - start).seconds() > 5.0) {
                    RCLCPP_WARN(get_logger(),
                                "deactivate_3d_scan not responding…");
                    result->status = "deactivate_3d_scan not responding\n";
                    goal_handle->abort(result);
                    return;
                }
                rclcpp::sleep_for(50ms);
            }
            msg_ = "Calculating Rotations";
            RCLCPP_INFO(get_logger(), msg_.c_str());

            pc_lines_ = lines_3d(img_array_, interval_, single_interval_);
            open3d::geometry::PointCloud pcd_;
            for (const auto &point : pc_lines_) {
                pcd_.points_.emplace_back(point);
            }
            auto boundbox = pcd_.GetMinimalOrientedBoundingBox(false);
            Eigen::Vector3d center = boundbox.GetCenter();
            rotmat_eigen_ = align_to_direction(boundbox.R_);

            planning_component_->setStartStateToCurrentState();
            moveit::core::RobotStatePtr current_state =
                moveit_cpp_->getCurrentState();
            Eigen::Isometry3d current_pose =
                current_state->getGlobalLinkTransform("tcp");
            target_pose_.header.frame_id =
                moveit_cpp_->getPlanningSceneMonitor()
                    ->getPlanningScene()
                    ->getPlanningFrame();
            target_pose_.pose = tf2::toMsg(current_pose);

            tf2::Matrix3x3 rotmat_tf_(
                rotmat_eigen_(0, 0), rotmat_eigen_(0, 1), rotmat_eigen_(0, 2),
                rotmat_eigen_(1, 0), rotmat_eigen_(1, 1), rotmat_eigen_(1, 2),
                rotmat_eigen_(2, 0), rotmat_eigen_(2, 1), rotmat_eigen_(2, 2));
            RCLCPP_INFO_STREAM(get_logger(), "\nAligned Rotation Matrix:\n"
                                                 << rotmat_eigen_);
            rotmat_tf_.getRPY(tmp_roll_, tmp_pitch_, tmp_yaw_);
            roll_ = -tmp_pitch_;
            pitch_ = tmp_roll_;
            yaw_ = tmp_yaw_;
            rotmat_tf_.setRPY(roll_, pitch_, yaw_);

            dz_ = (z_height_ - center[2]) / (px_per_mm * 1000.0);

            msg_ = std::format("Calculated:\n"
                               "    [Rotation] R:{:.2f} P:{:.2f} Y:{:.2f}\n"
                               "    [Center]   x:{:.2f}  y:{:.2f}  z:{:.2f}\n"
                               "    [Height]   dz:{:.4f}\n",
                               to_degree(roll_), to_degree(pitch_),
                               to_degree(yaw_), center[0], center[1], center[2],
                               dz_ * 1000);
            feedback->debug_msgs = msg_;
            goal_handle->publish_feedback(feedback);
            RCLCPP_INFO(get_logger(), msg_.c_str());

            if (tol_measure(roll_, pitch_, angle_tolerance_)) {
                angle_focused_ = true;
                msg_ = "=> Angle focused\n";
                feedback->debug_msgs = msg_;
                RCLCPP_INFO(get_logger(), msg_.c_str());
                goal_handle->publish_feedback(feedback);
            } else {
                if (!skip_angle_tolerance_) {
                    angle_focused_ = false;
                }
                if (skip_angle_tolerance_ && planning_) {
                    angle_focused_ = true;
                    planning_ = false;
                }
            }
            if (std::abs(dz_) < (z_tolerance_ / 1000.0)) {
                z_focused_ = true;
                msg_ = "=> Height focused\n";
                feedback->debug_msgs = msg_;
                RCLCPP_INFO(get_logger(), msg_.c_str());
                goal_handle->publish_feedback(feedback);
            } else {
                z_focused_ = false;
            }

            if (angle_focused_ && !z_focused_) {
                planning_ = true;
                target_pose_.pose.position.z += dz_;
                print_target(get_logger(), target_pose_.pose);
            }

            if (!angle_focused_) {
                planning_ = true;
                rotmat_tf_.getRotation(q_);
                tf2::fromMsg(target_pose_.pose.orientation, target_q_);
                target_q_ = target_q_ * q_;
                target_q_.normalize();
                target_pose_.pose.orientation = tf2::toMsg(target_q_);
                target_pose_.pose.position.z += dz_;
                print_target(get_logger(), target_pose_.pose);
            }

            if (planning_) {
                if (!skip_angle_tolerance_) {
                    planning_ = false;
                }
                planning_component_->setStartStateToCurrentState();

                moveit::core::RobotStatePtr cur_state =
                    moveit_cpp_->getCurrentState();
                Eigen::Isometry3d start_tcp =
                    cur_state->getGlobalLinkTransform("tcp");
                auto envelope = makeEnvelope(start_tcp, 0.05, M_PI);
                planning_component_->setPathConstraints(envelope);

                planning_component_->setGoal(target_pose_, "tcp");

                auto req = moveit_cpp::PlanningComponent::
                    MultiPipelinePlanRequestParameters(
                        shared_from_this(), {"pilz_ptp", "pilz_lin"});
                // auto stop_on_first =
                //     [](const PlanningComponent::PlanSolutions &sols,
                //        const auto &) { return sols.hasSuccessfulSolution();
                //        };
                auto choose_shortest =
                    [](const std::vector<planning_interface::MotionPlanResponse>
                           &sols) {
                        return *std::min_element(
                            sols.begin(), sols.end(),
                            [](const auto &a, const auto &b) {
                                if (a && b)
                                    return robot_trajectory::pathLength(
                                               *a.trajectory) <
                                           robot_trajectory::pathLength(
                                               *b.trajectory);
                                return static_cast<bool>(a);
                            });
                    };

                planning_interface::MotionPlanResponse plan_solution =
                    planning_component_->plan(req, choose_shortest);
                if (plan_solution) {
                    if (!goal_handle->is_active()) {
                        tem_->stopExecution(true);
                        planning_component_->setStartStateToCurrentState();
                        return;
                    }
                    if (goal_handle->is_canceling()) {
                        tem_->stopExecution(true);
                        result->status = "Cancel requested!\n";
                        goal_handle->canceled(result);
                        RCLCPP_INFO(get_logger(), "Cancel requested!");
                        planning_component_->setStartStateToCurrentState();
                        return;
                    }
                    bool execute_success =
                        moveit_cpp_->execute(plan_solution.trajectory);
                    if (execute_success) {
                        RCLCPP_INFO(get_logger(), "Execute Success!");
                        if (early_terminate_) {
                            angle_focused_ = true;
                            z_focused_ = true;
                            break;
                        }
                    } else {
                        RCLCPP_INFO(get_logger(), "Execute Failed!");
                        feedback->debug_msgs += "Execute Failed!\n";
                    }
                } else {
                    RCLCPP_INFO(get_logger(), "Planning failed!");
                    feedback->debug_msgs += "Planning failed!\n";
                }
                goal_handle->publish_feedback(feedback);
            }
        }

        msg_ = "Within tolerance or Early termination\n";
        feedback->debug_msgs = msg_;
        goal_handle->publish_feedback(feedback);
        start = now();
        if (!goal_handle->is_active()) {
            tem_->stopExecution(true);
            planning_component_->setStartStateToCurrentState();
            return;
        }
        if (goal_handle->is_canceling()) {
            tem_->stopExecution(true);
            feedback->debug_msgs = "Focus action canceled\n";
            result->status = "Focus action canceled\n";
            goal_handle->publish_feedback(feedback);
            goal_handle->canceled(result);
            return;
        }

        if (goal_handle->is_active()) {
            result->status = "Focus completed successfully\n";
            goal_handle->succeed(result);
            img_timer_->cancel();
            RCLCPP_INFO(get_logger(), "Focus action completed successfully.");
        }
    }

    void captureBackgroundCallback(
        [[maybe_unused]] const std::shared_ptr<std_srvs::srv::Trigger::Request>
            request,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        img_timer_->reset();
        cv::Mat frame = get_img();
        if (!frame.empty()) {
            std::string pkg_share =
                ament_index_cpp::get_package_share_directory("octa_ros");
            std::string bg_path = pkg_share + "/config/bg.jpg";
            cv::imwrite(bg_path.c_str(), frame);
            cv::imwrite("config/bg.jpg", frame);
            response->success = true;
        } else {
            RCLCPP_INFO(get_logger(),
                        "No image captured – background not saved");
            response->success = false;
        }
        img_timer_->cancel();
    }
};

int main(int argc, char **argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<FocusActionServer>();
    node->init();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
