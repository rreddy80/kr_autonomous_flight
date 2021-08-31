

#include <action_trackers/RunTrajectoryAction.h>
#include <actionlib/client/simple_action_client.h>
#include <actionlib/server/simple_action_server.h>
#include <fla_state_machine/ReplanAction.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <geometry_msgs/TransformStamped.h>
#include <geometry_msgs/Twist.h>
#include <kr_mav_msgs/PositionCommand.h>
#include <planning_ros_msgs/Path.h>
#include <planning_ros_msgs/PlanTwoPointAction.h>
#include <planning_ros_msgs/VoxelMap.h>
#include <planning_ros_utils/data_ros_utils.h>
#include <ros/ros.h>
#include <sensor_msgs/Temperature.h>
#include <std_msgs/Int64.h>
#include <tf/transform_datatypes.h>
#include <tf2_ros/transform_listener.h>
// #include <tf/transform_listener.h>
#include <geometry_msgs/PoseStamped.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <traj_opt_ros/msg_traj.h>
#include <traj_opt_ros/ros_bridge.h>

#include <boost/timer/timer.hpp>
#include <fla_state_machine/intersect_utils.hpp>
#include <fla_state_machine/traj_opt_utils.hpp>

// Timer stuff
using boost::timer::cpu_timer;
using boost::timer::cpu_times;

class RePlanner {
 public:
  RePlanner();
  int max_horizon_;
  bool active_{false};

  /**
   * @brief Set up replanner, get an initial plan and execute it
   * only run if the following three conditions are met:
   * if the planner is not active_
   * if ReplanGoalCb is already called
   * if local map callback is already called (path_crop needs local map)
   */
  void setup_replanner();

  /**
   * @brief Update status, where replan_server_ status is set as success if the
   * following two conditions are satisfied: (1)finished_replanning_ is true
   * (distance between user-given goal position and traj evaluated position is
   * less than 1.0) (2)the commanded position and traj end position is less than
   * 1e-3
   */
  void update_status();

 private:
  std::unique_ptr<
      actionlib::SimpleActionServer<fla_state_machine::ReplanAction>>
      replan_server_;
  std::unique_ptr<
      actionlib::SimpleActionClient<action_trackers::RunTrajectoryAction>>
      run_client_;
  std::unique_ptr<
      actionlib::SimpleActionClient<planning_ros_msgs::PlanTwoPointAction>>
      local_plan_client_;  // local plan action server client
  std::unique_ptr<
      actionlib::SimpleActionClient<planning_ros_msgs::PlanTwoPointAction>>
      global_plan_client_;  // global plan action server client
  boost::mutex mtx_;

  // Timing stuff
  ros::Publisher time_pub1;
  ros::Publisher time_pub2;
  cpu_timer timer;

  // tf_listener
  tf2_ros::Buffer tfBuffer;
  tf2_ros::TransformListener *tfListener;

  // reference frame names
  std::string map_frame_;   // map frame
  std::string odom_frame_;  // odom frame

  // // local cropped path pub for visualization
  // ros::Publisher cropped_path_pub_;

  // transformed global path pub for visualization
  ros::Publisher global_path_wrt_map_pub_;

  geometry_msgs::Pose pose_goal_;           // goal recorder
  geometry_msgs::Pose pose_goal_wrt_odom_;  // goal recorder (transformed to
                                            // account for odom drift)

  int waypoint_idx_;  // index of current goal in the array of goals (waypoints)
  std::vector<geometry_msgs::Pose> pose_goals_;  // an array of goals

  bool finished_replanning_{
      true};  // one of termination conditions (the other is that commanded
              // position and traj end position is less than 1e-3)
  bool do_setup_{false};  // two conditions: if not yet received any goal, don't
                          // do setup; if already setup once, don't do again.

  traj_opt::VecD cmd_pos_{traj_opt::VecD::Zero(4, 1)};  // position command
  boost::shared_ptr<traj_opt::Trajectory>
      last_traj_;  // record of latest trajectory
  planning_ros_msgs::SplineTrajectory last_traj_msg_;
  ros::NodeHandle nh_;
  ros::Subscriber cmd_sub_;
  ros::Subscriber local_map_sub_;
  planning_ros_msgs::VoxelMapConstPtr local_map_ptr_;

  // epoch is to record how many steps have been executed, the duration of one
  // epoch is the execution time, which is 1.0/replan_rate
  int last_plan_epoch_;
  ros::Subscriber epoch_sub_;

  // subscribe to global path
  ros::Subscriber global_path_sub_;

  // local-global framework related params
  double local_replan_rate_;  // should be set in the goal sent from the
                              // state_machine
  vec_Vec3f global_path_;  // recorder of path planned by global action server
  double local_timeout_duration_;  // local planner timeout duration
  Vec3f prev_start_pos_;  // replanning records: previous replanning start
                          // position

  // maximum trials of local replan allowed
  int max_local_trials_;
  double failed_local_trials_ = 0;
  unsigned int global_plan_counter_ = 0;
  double crop_radius_;    // local path crop radius (local path length will be
                          // this value)
  double crop_radius_z_;  // local path crop radius along z axis
  double close_to_final_dist_;
  float final_waypoint_threshold_;
  float waypoint_threshold_;
  bool avoid_obstacle_{true};

  fla_state_machine::ReplanResult critical_;  // create critical bug report

  /**
   * @brief Epoch callback function, triggered by epoch msg published by
   trajectory_tracker_upgraded, it will trigger the replan process (ONLY IF
   current epoch is different than the previously recorded one) by calling
   plan_trajectory and RunTrajectory functions

   */
  void EpochCb(const std_msgs::Int64 &msg);

  /**
   * @brief Command callback function, setting cmd_pos_ to be the commanded
   * position
   */
  void CmdCb(const kr_mav_msgs::PositionCommand &cmd);

  /**
   * @brief map callback, update local_map_ptr_
   */
  void LocalMapCb(const planning_ros_msgs::VoxelMap::ConstPtr &msg);

  /**
   * @brief Goal callback function
   */
  void ReplanGoalCb();

  /**
   * @brief Goal done callback function
   */
  void GlobalPathCb(const planning_ros_msgs::Path &path);

  /**
   * @brief Execute the planned trajectory, during which epoch will be published
   * by trajectory tracker and EpochCb will be triggered, which will trigger
   * replanning process
   */
  void RunTrajectory();

  /**
   * @brief Plan trajectory: (1) call global planner, (2) crop global path to
   * get local goal, (3) call local planner
   */
  bool PlanTrajectory(int horizon);

  /**
   * @brief Crop global path for local planner by intersecting it with local map
   * boundaries
   * @param path original path to crop
   * @param d length of the cropped path
   *
   */
  vec_Vec3f PathCrop(const vec_Vec3f &path);

  /**
   * @brief Crop global path for local planner with a fixed distance
   * @param path original path to crop
   * @param d length of the cropped path
   *
   */
  vec_Vec3f PathCrop(const vec_Vec3f &path, double crop_dist_xyz,
                     double crop_dist_z);

  /**
   * @brief Check if cropped path reaches the end of original path
   */
  bool CloseToFinal(const vec_Vec3f &original_path,
                    const vec_Vec3f &cropped_path,
                    double dist_threshold = 10.0);

  /**
   * @brief transform global path from odom frame to map frame
   */
  vec_Vec3f TransformGlobalPath(const vec_Vec3f &path_original);

  /**
   * @brief transform global path from map frame to odom frame
   */
  void TransformGlobalGoal();

  /**
   * @brief abort the replan process
   */
  void AbortReplan(void);
};

/**
 * @brief Goal done callback function
 */

void RePlanner::GlobalPathCb(const planning_ros_msgs::Path &path) {
  global_path_.clear();
  global_path_ = ros_to_path(path);  // extract the global path information
}

void RePlanner::EpochCb(const std_msgs::Int64 &msg) {
  if (msg.data < 0) {
    ROS_ERROR("[Replanner:] aborting mission because tracker failed!!!");
    ROS_ERROR("[Replanner:] aborting mission because tracker failed!!!");
    ROS_ERROR("[Replanner:] aborting mission because tracker failed!!!");
    ROS_ERROR("RePlanner has not received position cmd, failing");

    AbortReplan();
    return;
  }

  boost::mutex::scoped_lock lock(mtx_);
  static int epoch_old = -1;  // keep a record of last epoch

  // replan ONLY IF current epoch is different than the previously recorded one
  if (epoch_old != msg.data) {
    int horizon;
    if (epoch_old == -1) {
      horizon = 1;
    } else {
      // msg.data = current_epoch_ + int(std::floor(duration/execution_time)),
      // where duration = (t_now - started_).toSec() horizon = 1 +
      // (current_plan_epoch - last_plan_epoch). Duration of one epoch is
      // execution_time, which is 1.0/replan_rate
      horizon = msg.data - last_plan_epoch_ + 1;
    }
    epoch_old = msg.data;  // keep a record of last epoch

    // trigger replan, set horizon
    if (!finished_replanning_) {
      if (PlanTrajectory(horizon))
        // execute the replanned trajectory
        RunTrajectory();
    }
  }
  update_status();
}

void RePlanner::CmdCb(const kr_mav_msgs::PositionCommand &cmd) {
  boost::mutex::scoped_lock lock(mtx_);
  cmd_pos_(0) = cmd.position.x;
  cmd_pos_(1) = cmd.position.y;
  cmd_pos_(2) = cmd.position.z;
  cmd_pos_(3) = cmd.yaw;
}

// map callback, update local_map_
void RePlanner::LocalMapCb(const planning_ros_msgs::VoxelMap::ConstPtr &msg) {
  ROS_WARN_ONCE("[Replanner:] Got the local voxel map!");
  local_map_ptr_ = msg;
}

void RePlanner::ReplanGoalCb() {
  boost::mutex::scoped_lock lock(mtx_);
  // accept new goal (ref:
  // http://docs.ros.org/en/jade/api/actionlib/html/classactionlib_1_1SimpleActionServer.html#a4964ef9e28f5620e87909c41f0458ecb)
  auto goal = replan_server_->acceptNewGoal();
  local_replan_rate_ =
      goal->replan_rate;  // set local planner replan_rate to be the same as
                          // specified in goal (assigned in state machine)
  pose_goals_ = goal->p_finals;

  ROS_WARN_STREAM("[Replanner:] waypoints received, number of waypoints is:"
                  << pose_goals_.size());

  if (pose_goals_.empty()) {
    ROS_WARN("++++++++++++++++++++++++++++++++++++++++++++");
    ROS_WARN(
        "[Replanner:] waypoints list is empty, now use single goal intead!");
    ROS_WARN("++++++++++++++++++++++++++++++++++++++++++++");
    pose_goals_.push_back(goal->p_final);
  } else if (pose_goals_.size() > 1) {
    ROS_INFO(
        "[Replanner:] received more than one waypoints! Number of waypoints: "
        "%zu",
        pose_goals_.size());
  }
  waypoint_idx_ = 0;
  pose_goal_ = pose_goals_[waypoint_idx_];
  TransformGlobalGoal();

  avoid_obstacle_ = goal->avoid_obstacles;  // obstacle avoidance mode
                                            // (assigned in state machine)

  // check cmd
  if (cmd_pos_.norm() == 0) {
    ROS_ERROR("RePlanner has not received position cmd, failing");
    AbortReplan();
    return;
  }

  if (!active_)        // if not active_, do setup again
    do_setup_ = true;  // only run setup_replanner function after ReplanGoalCb
}

void RePlanner::setup_replanner() {
  if (!do_setup_ || active_) {
    return;
  }
  if (local_map_ptr_ == nullptr) {
    ROS_WARN(
        "[Replanner:] local_map_ptr_ is nullptr, local map not received "
        "yet!!!!!");
    return;
  }
  do_setup_ = false;  // only run setup_replanner once
  boost::mutex::scoped_lock lock(mtx_);

  ROS_INFO_STREAM("Setup replan");
  if (!avoid_obstacle_) {
    ROS_WARN("+++++++++++++++++++++++++++++++++++");
    ROS_WARN("Obstacle avoidance is disabled!!!!!");
    ROS_WARN("+++++++++++++++++++++++++++++++++++");
  }

  if (waypoint_threshold_ < final_waypoint_threshold_) {
    ROS_WARN(
        "waypoint_reach_threshold is set as smaller than "
        "final_waypoint_reach_threshold, this is not recommanded, now "
        "changing the final_waypoint_threshold to be the same as "
        "waypoint_threshold!");
    final_waypoint_threshold_ = waypoint_threshold_;
  }

  // Initial plan step 1: global plan
  // ########################################################################################################
  // get the distance from cmd pos to goal (waypoint) pos
  float x_diff = (float)std::abs(pose_goal_wrt_odom_.position.x - cmd_pos_(0));
  float y_diff = (float)std::abs(pose_goal_wrt_odom_.position.y - cmd_pos_(1));
  Vec2f xy_diff = Vec2f{pose_goal_wrt_odom_.position.x - cmd_pos_(0),
                        pose_goal_wrt_odom_.position.y - cmd_pos_(1)};
  float dist_cmd_to_goal = xy_diff.norm();
  if (dist_cmd_to_goal <= waypoint_threshold_) {
    if ((waypoint_idx_ >= (pose_goals_.size() - 1)) &&
        (dist_cmd_to_goal <= final_waypoint_threshold_)) {
      // exit replanning process if the final waypoint is reached
      ROS_WARN(
          "[Replanner:] Initial (and the only) waypoint is already close to "
          "the robot "
          "position, terminating the replanning process!");
      fla_state_machine::ReplanResult success;
      success.status = fla_state_machine::ReplanResult::SUCCESS;
      active_ = false;
      if (replan_server_->isActive()) {
        replan_server_->setSucceeded(success);
      }
      last_traj_ = boost::shared_ptr<traj_opt::Trajectory>();
      last_traj_msg_ = planning_ros_msgs::SplineTrajectory();
      return;
    } else if (waypoint_idx_ < (pose_goals_.size() - 1)) {
      // take the next waypoint if the intermidiate waypoint is reached
      ROS_WARN(
          "[Replanner:] Initial waypoint is already close to the robot "
          "position, continuing "
          "with the next waypoint!");
      ++waypoint_idx_;
      pose_goal_ = pose_goals_[waypoint_idx_];
      TransformGlobalGoal();
    }
  };

  // set goal
  planning_ros_msgs::PlanTwoPointGoal global_tpgoal;
  global_tpgoal.p_final = pose_goal_wrt_odom_;
  global_tpgoal.avoid_obstacles = avoid_obstacle_;

  // send goal to global plan action server
  global_plan_client_->sendGoal(
      global_tpgoal);  // only send goal, because global plan server is
                       // subscribing to odom and use that as start
  // global initial plan timeout duration
  bool global_finished_before_timeout =
      global_plan_client_->waitForResult(ros::Duration(4.0));
  // check result of global plan
  if (!global_finished_before_timeout) {
    ROS_ERROR("initial global planning timed out");
    AbortReplan();
    return;
  }
  auto global_result = global_plan_client_->getResult();
  if (global_result->success) {
    global_path_ = ros_to_path(
        global_result->path);  // extract the global path information
    ROS_WARN("initial global plan succeeded!");
  } else {
    ROS_WARN("initial global plan failed!");
    AbortReplan();
    return;
  }

  //  Initial plan step 2: Crop global path to get local goal
  //  #################################################################################
  vec_Vec3f global_path_wrt_map = TransformGlobalPath(global_path_);
  vec_Vec3f path_cropped_wrt_odom = PathCrop(global_path_);

  if (path_cropped_wrt_odom.size() == 0) {
    ROS_ERROR("[Replanner:] Path crop failed!");
    AbortReplan();
    return;
  }

  bool close_to_final_goal;
  if (close_to_final_dist_ > 0) {
    close_to_final_goal =
        CloseToFinal(global_path_, path_cropped_wrt_odom, close_to_final_dist_);
  } else {
    close_to_final_dist_ = false;
  }
  // Initial plan step 3: local plan
  // ##########################################################################################################
  // set goal
  planning_ros_msgs::PlanTwoPointGoal local_tpgoal;
  fla_state_machine::VecToPose(
      cmd_pos_, &local_tpgoal.p_init);  // use current position command as the
                                        // start position for local planner
  // initialize prev_start_pos_ for replan purpose
  prev_start_pos_(0) = cmd_pos_(0);
  prev_start_pos_(1) = cmd_pos_(1);
  prev_start_pos_(2) = cmd_pos_(2);

  // set vars
  local_tpgoal.epoch = 1;
  local_tpgoal.avoid_obstacles = avoid_obstacle_;
  local_tpgoal.execution_time = ros::Duration(1.0 / local_replan_rate_);
  // if close_to_final_goal, we need to check velocity tolerance as well
  local_tpgoal.check_vel = close_to_final_goal;
  // set p_final to be path_cropped_wrt_odom.back(), which is exactly at
  // accumulated distance d from the robot (unless path is shorter than d,
  // crop_end will be default as the end of path)
  Vec3f local_goal = path_cropped_wrt_odom.back();
  local_tpgoal.p_final.position.x = local_goal(0);
  local_tpgoal.p_final.position.y = local_goal(1);
  local_tpgoal.p_final.position.z = local_goal(2);

  // send goal to local trajectory plan action server
  local_plan_client_->sendGoal(local_tpgoal);

  // wait for result (initial timeout duration can be large because robot is not
  // moving)
  bool local_finished_before_timeout =
      local_plan_client_->waitForResult(ros::Duration(2.0));

  // reset the counter
  // TODO(xu:) double check this is working properlly
  failed_local_trials_ = 0;

  if (!local_finished_before_timeout) {
    // check result of local plan
    ROS_ERROR("Initial local planning timed out");
    ROS_ERROR("Initial local planning timout duration is set as 2.0");
    AbortReplan();
    return;
  }
  auto local_result = local_plan_client_->getResult();
  if (!local_result->success) {
    ROS_ERROR("Initial local planning failed to find a local trajectory!");
    AbortReplan();
    return;
  }

  // get the result
  last_traj_msg_ = local_result->traj;
  last_traj_ = boost::make_shared<traj_opt::MsgTrajectory>(
      traj_opt::TrajDataFromSplineTrajectory(local_result->traj));
  last_plan_epoch_ = local_result->epoch;

  // setup state vars
  finished_replanning_ = false;

  // Initial plan step 4: execute the planned trajectory, during which
  // replanning process will also be triggered
  ROS_INFO("Started replanning!");
  RunTrajectory();
}

void RePlanner::RunTrajectory() {
  if (!replan_server_->isActive()) {
    return;
  }

  // set up run goal
  action_trackers::RunTrajectoryGoal rungoal;
  rungoal.traj = last_traj_msg_;
  rungoal.epoch = last_plan_epoch_;
  rungoal.replan_rate = local_replan_rate_;

  // call the trajectory tracker
  run_client_->sendGoal(rungoal);
  // ROS_INFO("Sent Trajectory!");
  double tracker_timeout_dur = 0.5;
  bool tracker_finished_before_timeout =
      run_client_->waitForResult(ros::Duration(tracker_timeout_dur));
  if (!tracker_finished_before_timeout) {
    ROS_ERROR("Tracker aborted or timeout!");
    fla_state_machine::ReplanResult abort;
    abort.status = fla_state_machine::ReplanResult::ABORT_FULL_MISSION;
    active_ = false;
    replan_server_->setAborted(abort);
  }
}

bool RePlanner::PlanTrajectory(int horizon) {
  // horizon = 1 + (current_plan_epoch - last_plan_epoch),
  // where duration of one epoch is execution_time, which is 1.0/replan_rate
  if (horizon > max_horizon_) {
    ROS_ERROR(
        "Planning horizon is larger than max_horizon_, aborting the mission!");
    fla_state_machine::ReplanResult abort;
    abort.status = fla_state_machine::ReplanResult::DYNAMICALLY_INFEASIBLE;
    active_ = false;
    if (replan_server_->isActive()) {
      replan_server_->setAborted(abort);
    }
    return false;
  }

  // set global goal
  planning_ros_msgs::PlanTwoPointGoal global_tpgoal;

  // Important: for two reference frame system, we need to apply transform from
  // slam to odom so that the global goal in odom frame is adjusted to account
  // for the drift
  global_tpgoal.p_final = pose_goal_wrt_odom_;
  global_tpgoal.avoid_obstacles = avoid_obstacle_;

  // set local goal
  planning_ros_msgs::PlanTwoPointGoal local_tpgoal;
  double eval_time =
      double(horizon) /
      local_replan_rate_;  // calculate evaluation time, i.e., beginning of
                           // current plan epoch (in seconds)
  // make the end of last trajectory consistent with the start of current
  // trajectory
  fla_state_machine::EvaluateToMsgs(last_traj_, eval_time, &local_tpgoal.p_init,
                                    &local_tpgoal.v_init, &local_tpgoal.a_init,
                                    &local_tpgoal.j_init);
  Vec3f start_pos;
  start_pos = pose_to_eigen(local_tpgoal.p_init);

  // Replan step 1: Global plan
  // ########################################################################################################
  // send goal to global plan server to replan (new goal will preempt old goals)
  if ((global_plan_counter_ % 2) == 0) {
    global_plan_client_->sendGoal(global_tpgoal);
    ROS_INFO_THROTTLE(2,
                      "[Replanner:] global replan called at half of the "
                      "frequency of local replan");
    // this is just for visualization purposes
    vec_Vec3f global_path_wrt_map = TransformGlobalPath(global_path_);
  }
  global_plan_counter_++;
  prev_start_pos_ = start_pos;  // keep updating prev_start_pos_

  //  Replan step 2: Crop global path to get local goal
  //  #################################################################################

  // ROS_WARN_STREAM("++++ total_crop_dist = " << crop_dist);
  vec_Vec3f path_cropped_wrt_odom = PathCrop(global_path_);
  if (path_cropped_wrt_odom.size() == 0) {
    ROS_ERROR("[Replanner:] Path crop failed!");
    AbortReplan();
    return false;
  }

  bool close_to_final_goal;
  if (close_to_final_dist_ > 0) {
    close_to_final_goal =
        CloseToFinal(global_path_, path_cropped_wrt_odom, close_to_final_dist_);
  } else {
    close_to_final_dist_ = false;
  }

  // Replan step 3: local plan
  // ##########################################################################################################
  // set vars
  local_tpgoal.avoid_obstacles = avoid_obstacle_;
  local_tpgoal.execution_time = ros::Duration(1.0 / local_replan_rate_);
  local_tpgoal.epoch = last_plan_epoch_ + horizon;
  // if close_to_final_goal, we need to check velocity tolerance as well
  local_tpgoal.check_vel = close_to_final_goal;
  // change p_final to be path_cropped_wrt_odom.back(), which is exactly at
  // accumulated distance d from the robot (unless path is shorter than d,
  // crop_end will be default as the end of path)
  Vec3f local_goal = path_cropped_wrt_odom.back();
  local_tpgoal.p_final.position.x = local_goal(0);
  local_tpgoal.p_final.position.y = local_goal(1);
  local_tpgoal.p_final.position.z = local_goal(2);
  local_tpgoal.eval_time = eval_time;
  local_tpgoal.last_traj = last_traj_msg_;

  timer.start();
  // send goal to local trajectory plan action server
  local_plan_client_->sendGoal(local_tpgoal);

  // wait for result
  bool local_finished_before_timeout =
      local_plan_client_->waitForResult(ros::Duration(local_timeout_duration_));

  // timer stuff
  sensor_msgs::Temperature tmsg2;
  tmsg2.header.stamp = ros::Time::now();
  tmsg2.header.frame_id = map_frame_;
  // millisecond
  tmsg2.temperature = static_cast<double>(timer.elapsed().wall) / 1e6;
  // ROS_WARN("[local_planner_time]: %f", tmsg2.temperature);
  // time_pub2.publish(tmsg2);

  // check result of local plan
  bool local_succeeded = true;
  if (!local_finished_before_timeout) {
    failed_local_trials_ += 1;
    ROS_WARN_STREAM(
        "Local planner timed out, trying to replan...  (local planner already "
        "failed "
        << failed_local_trials_
        << " times, total allowed trails: " << max_local_trials_ << ")");
    local_succeeded = false;
  } else {
    auto local_result = local_plan_client_->getResult();
    if (local_result->success) {
      last_traj_ = boost::make_shared<traj_opt::MsgTrajectory>(
          traj_opt::TrajDataFromSplineTrajectory(local_result->traj));
      last_traj_msg_ = local_result->traj;
      last_plan_epoch_ = local_result->epoch;
      // ROS_INFO_STREAM("Got local plan with epoch " << last_plan_epoch_);
      failed_local_trials_ = 0;  // reset this
      return true;
    } else {
      failed_local_trials_ += 1;
      ROS_WARN_STREAM(
          "Local planner failed, trying to replan...  (local planner already "
          "failed "
          << failed_local_trials_
          << " times, total allowed trails: " << max_local_trials_ << ")");
      local_succeeded = false;
    }
  }

  if (failed_local_trials_ >= max_local_trials_ - 1) {
    // if (waypoint_idx_ >= (pose_goals_.size() - 1)) {
    // if this is the final waypoint, abort full mission
    AbortReplan();
    // } else {
    // otherwise, allow one more try with the next waypoint
    // TODO(xu): maybe abort full mission is a better choice if we want to
    // visit every waypoint?

    // ++waypoint_idx_;

    // TODO(xu:) handle this in state_machine instead
    // ROS_WARN_STREAM(
    //     "Current intermidiate waypoint leads to local planner timeout, for "
    //     << max_local_trials_ - 1
    //     << "times giving one last try with the next waypoint, "
    //        "whose index is: "
    //     << waypoint_idx_);

    // failed_local_trials_ =
    //     failed_local_trials_ - 0.5;  // - 0.5 so that if we timeout again,
    //     the
    //                                  // replanner will be aborted
    // }
    return false;
  }

  // return true if local planner does not time out AND local_result is success
  return local_succeeded;
}

void RePlanner::update_status() {
  // check for termination
  if (last_traj_ != NULL) {
    traj_opt::VecD pos_goal = traj_opt::VecD::Zero(4, 1);
    traj_opt::VecD pos_final, pos_finaln;
    pos_goal(0) = pose_goal_wrt_odom_.position.x;
    pos_goal(1) = pose_goal_wrt_odom_.position.y;

    // evaluate traj at 1.0/local_replan_rate_, output recorded to pos_finaln,
    // refer to msg_traj.cpp.
    last_traj_->evaluate(1.0 / local_replan_rate_, 0,
                         pos_finaln);  // TODO(mike) {fix this}.

    pos_final = fla_state_machine::Make4d(pos_finaln);
    pos_final(2) = 0;
    pos_final(3) = 0;

    // check if goal and traj evaluated position is less than threshold
    // get the distance from current pos to goal (waypoint) pos
    double dist_cmd_to_goal = (pos_goal - pos_final).norm();
    if (dist_cmd_to_goal <= waypoint_threshold_) {
      if ((waypoint_idx_ >= (pose_goals_.size() - 1)) &&
          (dist_cmd_to_goal <= final_waypoint_threshold_)) {
        // finished_replanning_ is set as true if this is the final waypoint
        finished_replanning_ = true;
        ROS_INFO_STREAM(
            "Final waypoint reached! The distance threshold is set as: "
            << final_waypoint_threshold_ << " Total " << pose_goals_.size()
            << " waypoints received");
      } else if (waypoint_idx_ < (pose_goals_.size() - 1)) {
        // take the next waypoint if the intermidiate waypoint is reached
        ++waypoint_idx_;
        ROS_INFO_STREAM(
            "Intermidiate waypoint reached, continue to the next waypoint, "
            "whose index is: "
            << waypoint_idx_
            << "The distance threshold is set as:" << waypoint_threshold_);
        pose_goal_ = pose_goals_[waypoint_idx_];
        TransformGlobalGoal();
      }
    };
  }

  // check for termination, evaluating trajectory and see if it's close to
  // pos_final
  if (last_traj_ != NULL && finished_replanning_) {
    ROS_INFO_STREAM_THROTTLE(1, "Waiting for traj termination");
    traj_opt::VecD pos_no_yaw = cmd_pos_;
    traj_opt::VecD pos_final, pos_finaln;
    // evaluate traj at last_traj_->getTotalTime(), output recorded to
    // pos_finaln,
    last_traj_->evaluate(last_traj_->getTotalTime(), 0, pos_finaln);
    last_traj_->evaluate(last_traj_->getTotalTime(), 0, pos_finaln);
    last_traj_->evaluate(last_traj_->getTotalTime(), 0, pos_finaln);
    pos_final = fla_state_machine::Make4d(pos_finaln);

    pos_no_yaw(2) = 0;
    pos_no_yaw(3) = 0;
    pos_final(2) = 0;
    pos_final(3) = 0;
    ROS_INFO_STREAM_THROTTLE(
        1, "Termination error: " << (pos_no_yaw - pos_final).norm());

    // replan_server_ set as success if the commanded position and traj end
    // position is less than 1e-3
    if ((pos_no_yaw - pos_final).norm() <= 1e-3) {
      fla_state_machine::ReplanResult success;
      success.status = fla_state_machine::ReplanResult::SUCCESS;
      active_ = false;
      if (replan_server_->isActive()) {
        replan_server_->setSucceeded(success);
      }
      ROS_INFO("RePlanner success!!");
      last_traj_ = boost::shared_ptr<traj_opt::Trajectory>();
      last_traj_msg_ = planning_ros_msgs::SplineTrajectory();
      return;
    }
  }

  // fla_state_machine::ReplanResult in_progress;
  // in_progress.status = fla_state_machine::ReplanResult::IN_PROGRESS;

  if (replan_server_->isActive()) {
    active_ = true;
    // replan_server_->setSucceeded(in_progress);
  }
}

vec_Vec3f RePlanner::PathCrop(const vec_Vec3f &path) {
  if (path.size() < 2) {
    ROS_WARN("[Replanner:] global path has <= 1 waypoints. Check!");
    // return empty
    return vec_Vec3f{};
  }

  float lower_x = local_map_ptr_->origin.x;
  float lower_y = local_map_ptr_->origin.y;
  float lower_z = local_map_ptr_->origin.z;

  // shrink the local map boundary a bit to guarantee the goal is inside
  // boundary (insead of on the boundary)
  int num_shrink_voxels = 1;
  float upper_x = lower_x + (local_map_ptr_->dim.x - num_shrink_voxels) *
                                local_map_ptr_->resolution;
  float upper_y = lower_y + (local_map_ptr_->dim.y - num_shrink_voxels) *
                                local_map_ptr_->resolution;
  float upper_z = lower_z + (local_map_ptr_->dim.z - num_shrink_voxels) *
                                local_map_ptr_->resolution;
  Vec3f map_lower = Vec3f{lower_x, lower_y, lower_z};
  Vec3f map_upper = Vec3f{upper_x, upper_y, upper_z};

  bool start_in_box =
      fla_state_machine::CheckPointInBox(map_lower, map_upper, path[0]);
  if (!start_in_box) {
    ROS_ERROR(
        "[Replanner:] global path start is outside local voxel map. Check!");
    // return empty
    return vec_Vec3f{};
  }

  // the end of cropped path will be at the intersection between global path and
  // local map (if all segments of global path is inside the local map, it will
  // be default as the end of path)
  vec_Vec3f cropped_path{};
  // first, include the start of the global path
  cropped_path.push_back(path[0]);

  // add path segments until the intersection is found
  Vec3f intersect_pt;
  bool intesected;
  for (unsigned int i = 1; i < path.size(); i++) {
    intesected = fla_state_machine::IntersectLineBox(
        map_lower, map_upper, path[i - 1], path[i], intersect_pt);
    if (intesected) {
      // intersects, add the intersection
      cropped_path.push_back(intersect_pt);
      break;
    } else {
      // does not intersect, add the end current segment to cropped path
      cropped_path.push_back(path[i]);
    }
  }

  // // publish for visualization
  // planning_ros_msgs::Path local_path_msg_ = path_to_ros(cropped_path);
  // local_path_msg_.header.frame_id = map_frame_;
  // cropped_path_pub_.publish(local_path_msg_);

  return cropped_path;
}

vec_Vec3f RePlanner::TransformGlobalPath(const vec_Vec3f &path_original) {
  // get the latest tf from map to odom reference frames
  geometry_msgs::TransformStamped transformStamped;

  try {
    transformStamped = tfBuffer.lookupTransform(
        map_frame_, odom_frame_, ros::Time(0), ros::Duration(0.01));
  } catch (tf2::TransformException &ex) {
    ROS_ERROR("[Replanner:] Failed to get tf from %s to %s",
              odom_frame_.c_str(), map_frame_.c_str());
    // AbortReplan(); // no need to abort plan since this is just for
    // visualization return original path
    return path_original;
  }

  geometry_msgs::Pose map_to_odom;
  map_to_odom.position.x = transformStamped.transform.translation.x;
  map_to_odom.position.y = transformStamped.transform.translation.y;
  map_to_odom.position.z = transformStamped.transform.translation.z;
  map_to_odom.orientation.w = transformStamped.transform.rotation.w;
  map_to_odom.orientation.x = transformStamped.transform.rotation.x;
  map_to_odom.orientation.y = transformStamped.transform.rotation.y;
  map_to_odom.orientation.z = transformStamped.transform.rotation.z;

  // TF transform from the map frame to odom frame
  auto map_to_odom_tf = toTF(map_to_odom);
  Vec3f waypoint_wrt_map;

  vec_Vec3f path_wrt_map;
  for (unsigned int i = 0; i < path_original.size(); i++) {
    // apply TF on current waypoint
    waypoint_wrt_map = map_to_odom_tf * path_original[i];
    path_wrt_map.push_back(waypoint_wrt_map);
  }

  // publish transformed global path for visualization
  planning_ros_msgs::Path path_wrt_map_msg = path_to_ros(path_wrt_map);
  path_wrt_map_msg.header.frame_id = map_frame_;
  global_path_wrt_map_pub_.publish(path_wrt_map_msg);
  return path_wrt_map;
}

void RePlanner::TransformGlobalGoal() {
  // get the latest tf from map to odom reference frames
  geometry_msgs::TransformStamped transformStamped;
  try {
    // TF transform from odom frame to the map frame
    transformStamped = tfBuffer.lookupTransform(
        odom_frame_, map_frame_, ros::Time(0), ros::Duration(0.01));
  } catch (tf2::TransformException &ex) {
    ROS_ERROR("[Replanner:] Failed to get tf from %s to %s", map_frame_.c_str(),
              odom_frame_.c_str());
    AbortReplan();
  }
  geometry_msgs::PoseStamped pose_in;
  geometry_msgs::PoseStamped pose_out;
  pose_in.pose = pose_goal_;
  tf2::doTransform(pose_in, pose_out, transformStamped);
  pose_goal_wrt_odom_ = pose_out.pose;
  pose_goal_wrt_odom_.position.z = pose_goal_.position.z;
  ROS_INFO_THROTTLE(
      3,
      "[Replanner:] when transforming global goal, keeping z-axis value the "
      "same to guaranteed it's still within the voxel map");
}

vec_Vec3f RePlanner::PathCrop(const vec_Vec3f &path, double crop_dist_xyz,
                              double crop_dist_z) {
  // return nonempty
  // precondition
  if (path.size() < 2 || crop_dist_xyz < 0 || crop_dist_z < 0) {
    return path;
  }

  double d = crop_dist_xyz;
  double dz = crop_dist_z;

  vec_Vec3f cropped_path;
  // crop_end will be exactly at accumulated distance d from the robot
  // (unless path is shorter than d crop_end will be default as the end of
  // path)
  Vec3f crop_end = path.back();

  double dist = 0;
  double dist_z = 0;

  // add path segments until the accumulated length is farther than distance d
  // from the robot
  for (unsigned int i = 1; i < path.size(); i++) {
    if (dist_z + abs(path[i][2] - path[i - 1][2]) > dz) {
      auto seg_normalized = (path[i] - path[i - 1]).normalized();
      double remaining_crop_dist = (dz - dist_z) / abs(seg_normalized[2]);
      // make sure don't crop more than d

      double min_crop_dist = std::min(d - dist, remaining_crop_dist);
      crop_end =
          path[i - 1] + min_crop_dist * (path[i] - path[i - 1]).normalized();
      cropped_path.push_back(path[i - 1]);
      break;
    } else if (dist + (path[i] - path[i - 1]).norm() > d) {
      // assign end to be exactly at accumulated distance d from the robot
      crop_end =
          path[i - 1] + (d - dist) * (path[i] - path[i - 1]).normalized();
      cropped_path.push_back(path[i - 1]);
      break;
    } else {
      dist += (path[i] - path[i - 1]).norm();
      dist_z += abs(path[i][2] - path[i - 1][2]);
      cropped_path.push_back(path[i - 1]);
    }
  }

  // path is shorter than d crop_end will be default as the end of path
  if ((cropped_path.back() - crop_end).norm() > 1e-1)
    cropped_path.push_back(crop_end);

  // post condition
  // CHECK(glog) cropped_path is not empty
  return cropped_path;
}

bool RePlanner::CloseToFinal(const vec_Vec3f &original_path,
                             const vec_Vec3f &cropped_path,
                             double dist_threshold) {
  // precondition: original_path and cropped_path are non-empty
  if (original_path.size() < 2 || cropped_path.size() < 2) {
    return true;
  }

  Vec3f original_goal_pos = original_path.back();
  Vec3f cropped_goal_pos = cropped_path.back();

  if ((cropped_goal_pos - original_goal_pos).norm() < dist_threshold) {
    return true;
  } else {
    return false;
  }
}

void RePlanner::AbortReplan(void) {
  active_ = false;
  if (replan_server_->isActive()) {
    replan_server_->setAborted(critical_);
  }
  ROS_ERROR("[Replanner:] Replanning terminated!!");
}

RePlanner::RePlanner() : nh_("~") {
  tfListener = new tf2_ros::TransformListener(tfBuffer);

  ros::NodeHandle priv_nh(nh_, "local_global_server");

  time_pub1 = priv_nh.advertise<sensor_msgs::Temperature>(
      "/timing/replanner/global_replan", 1);
  time_pub2 = priv_nh.advertise<sensor_msgs::Temperature>(
      "/timing/replanner/local_replan", 1);

  // cropped_path_pub_ =
  //     priv_nh.advertise<planning_ros_msgs::Path>("cropped_local_path", 1,
  //     true);

  global_path_wrt_map_pub_ = priv_nh.advertise<planning_ros_msgs::Path>(
      "global_path_wrt_map", 1, true);

  priv_nh.param("max_horizon", max_horizon_, 5);
  priv_nh.param("crop_radius", crop_radius_, 10.0);
  priv_nh.param("crop_radius_z", crop_radius_z_, 2.0);
  priv_nh.param("close_to_final_dist", close_to_final_dist_, 10.0);
  priv_nh.param("final_goal_reach_threshold", final_waypoint_threshold_, 5.0f);
  priv_nh.param("waypoint_reach_threshold", waypoint_threshold_, 10.0f);
  priv_nh.param("local_plan_timeout_duration", local_timeout_duration_, 1.0);
  priv_nh.param("max_local_plan_trials", max_local_trials_, 3);
  priv_nh.param("odom_frame", odom_frame_, std::string("odom"));
  priv_nh.param("map_frame", map_frame_, std::string("map"));

  // replan action server
  replan_server_.reset(
      new actionlib::SimpleActionServer<fla_state_machine::ReplanAction>(
          nh_, "replan", false));

  // plan global path action client, auto spin option set to true
  global_plan_client_.reset(
      new actionlib::SimpleActionClient<planning_ros_msgs::PlanTwoPointAction>(
          nh_, "plan_global_path", true));

  // plan local trajectory action client, auto spin option set to true
  local_plan_client_.reset(
      new actionlib::SimpleActionClient<planning_ros_msgs::PlanTwoPointAction>(
          nh_, "plan_local_trajectory", true));

  // run trajectory action client
  run_client_.reset(
      new actionlib::SimpleActionClient<action_trackers::RunTrajectoryAction>(
          nh_, "execute_trajectory", true));

  // subscriber of position command
  // command callback: setting cmd_pos_ to be the commanded position
  cmd_sub_ = nh_.subscribe("position_cmd", 1, &RePlanner::CmdCb, this);
  local_map_sub_ =
      nh_.subscribe("local_voxel_map", 2, &RePlanner::LocalMapCb, this);
  local_map_ptr_ = nullptr;

  // subscriber of epoch command, epoch is published by trajectory tracker
  // epoch callback: trigger replan, set horizon
  epoch_sub_ = nh_.subscribe("epoch", 1, &RePlanner::EpochCb, this);

  // subscriber of global path
  global_path_sub_ =
      nh_.subscribe("global_path", 1, &RePlanner::GlobalPathCb, this);

  // create critical bug report
  critical_.status = fla_state_machine::ReplanResult::CRITICAL_ERROR;

  // Goal callback
  replan_server_->registerGoalCallback(
      boost::bind(&RePlanner::ReplanGoalCb, this));

  replan_server_->start();
}

int main(int argc, char **argv) {
  ros::init(argc, argv, "replanner");
  ros::NodeHandle nh;
  RePlanner replanner;
  ros::Rate r(5);  // Should be at least at the rate of local_replan_rate_!
  while (nh.ok()) {
    r.sleep();
    replanner.setup_replanner();  // this function will only run AFTER the
                                  // ReplanGoalCb, and will only run ONCE
    replanner.update_status();
    ros::spinOnce();
  }
}
